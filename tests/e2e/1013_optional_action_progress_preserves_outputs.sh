#!/usr/bin/env bash
# requires: gcc unix-shell python3
set -e
export MCPP="${MCPP:-mcpp}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
export PROGRESS_TEST_WORK="$work"
python3 - <<'PY'
from pathlib import Path
import json,os,pty,subprocess,sys,time,select,fcntl,termios,struct,signal
work=Path(os.environ['PROGRESS_TEST_WORK']);app=work/'app';(app/'src').mkdir(parents=True)
(app/'mcpp.toml').write_text('[package]\nname="action-progress"\nversion="0.1.0"\n')
(app/'src/main.cpp').write_text('extern "C" int answer();\nint main(){return answer()==42?0:1;}\n')
(app/'answer.c').write_text('int answer(void){return 42;}\n')
(app/'worker.py').write_text('''from pathlib import Path
import json,os,subprocess,sys,time
mode,output,compiler,source,proof=sys.argv[1:]
channel=os.environ.get('MCPP_ACTION_PROGRESS_FILE','')
Path(proof).write_text(channel)
Path(proof+".output").write_text(output)
if channel:
 p=Path(channel)
 for seq in range(3):
  temporary=p.with_suffix('.tmp')
  temporary.write_text(json.dumps(dict(v=1,seq=seq,phase='internal-'+mode,done=seq,total=2)))
  os.replace(temporary,p)
  time.sleep(.25)
if mode=='slow': time.sleep(30)
p=Path(output);p.parent.mkdir(parents=True,exist_ok=True)
if mode=='object':
 subprocess.run([compiler,'-x','c','-c',source,'-o',output],check=True)
elif mode=='prepare':
 p.mkdir(exist_ok=True);(p/'installed.txt').write_text('installed')
elif mode in ('source','artifact'):
 p.write_text('generated')
elif mode=='failure':
 sys.exit(7)
''')
def build_program(enabled=True,failure=False,slow=False):
 return '''import std;
import mcpp;
int main(){
 for(const auto* role : {mcpp::roles::check,mcpp::roles::prepare,mcpp::roles::source,mcpp::roles::object,mcpp::roles::artifact}) {
  const std::string name=role;
  const auto output=std::string(mcpp::out_dir())+"/"+name+(name=="object"?".o":".out");
  const auto proof=std::string(mcpp::manifest_dir())+"/proof-"+name;
  const auto source=std::string(mcpp::manifest_dir())+"/answer.c";
  const auto worker=std::string(mcpp::manifest_dir())+"/worker.py";
  const auto mode=SLOW && name=="check"?"slow":FAILURE && name=="check"?"failure":role;
  mcpp::action a;a.id=role;a.role=role;a.description=role;a.progress=ENABLED;
  a.arg("python3").arg(worker.c_str()).arg(mode);
  const auto directory=std::string(mcpp::out_dir())+"/installed";
  a.arg(name=="prepare"?directory.c_str():output.c_str()).arg(mcpp::tool("cc")).arg(source.c_str()).arg(proof.c_str());
  a.input(worker.c_str()).input(source.c_str()).output(output.c_str());
  if(name=="prepare")a.output_dir(directory.c_str());
  a.submit();
 }
}
'''.replace('ENABLED','true' if enabled else 'false').replace('FAILURE','true' if failure else 'false').replace('SLOW','true' if slow else 'false')
# Human output through a real terminal exercises the UI's asynchronous poll,
# while ninja is still buffering the command's stdout.
(app/'build.mcpp').write_text(build_program())
env=dict(os.environ,TERM='xterm',MCPP_PROGRESS='plain')
def terminal_build(extra=()):
 master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,160,0,0))
 process=subprocess.Popen([env['MCPP'],'build',*extra],cwd=app,stdout=slave,stderr=slave,env=env);os.close(slave)
 data=bytearray();deadline=time.monotonic()+180
 while process.poll() is None or select.select([master],[],[],0)[0]:
  if time.monotonic()>deadline:process.kill();raise AssertionError('progress build timed out')
  if select.select([master],[],[],.1)[0]:
   try:chunk=os.read(master,65536)
   except OSError:break
   if not chunk:break
   data.extend(chunk)
 os.close(master);assert process.wait()==0,data.decode(errors='replace')
 return data.decode(errors='replace')
text=terminal_build()
assert 'internal-' in text,text
channels=[(app/('proof-'+role)).read_text() for role in ('check','prepare','source','object','artifact')]
assert all(channels) and len(set(channels))==5,channels
assert all(not Path(channel).exists() for channel in channels),'execution channels survived the build'
proof_times={role:(app/('proof-'+role)).stat().st_mtime_ns for role in ('check','prepare','source','object','artifact')}
subprocess.run([env['MCPP'],'run'],cwd=app,env=env,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
assert all((app/('proof-'+role)).stat().st_mtime_ns==stamp for role,stamp in proof_times.items()),'runtime channels dirtied unchanged actions'
# Verbose mode also polls the channel while the nested command is running.
(app/'worker.py').touch()
verbose_text=terminal_build(('--verbose',))
assert 'internal-' in verbose_text,verbose_text
# Turning telemetry off leaves ordinary source/object outputs usable.
(app/'build.mcpp').write_text(build_program(enabled=False))
r=subprocess.run([env['MCPP'],'build','--message-format','json'],cwd=app,env=env,capture_output=True,text=True)
assert r.returncode==0,r.stdout+r.stderr
rows=[json.loads(line) for line in r.stdout.splitlines() if line.strip()]
assert rows and rows[-1]['kind']=='mcpp.build',rows
assert all((app/('proof-'+role)).read_text()=='' for role in ('check','prepare','source','object','artifact'))
subprocess.run([env['MCPP'],'run'],cwd=app,env=env,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
# Internal 2/2 followed by failure cannot create a successful check stamp.
previous_check=Path((app/'proof-check.output').read_text())
previous_stamp_time=previous_check.stat().st_mtime_ns
(app/'build.mcpp').write_text(build_program(failure=True))
r=subprocess.run([env['MCPP'],'build'],cwd=app,env=env,capture_output=True,text=True)
assert r.returncode!=0,'the internal completion count hid exit 7'
failed_check=Path((app/'proof-check.output').read_text())
assert not failed_check.exists() or (failed_check==previous_check and failed_check.stat().st_mtime_ns==previous_stamp_time),'failed check refreshed its success stamp'
# Cancellation ends the execution channel without refreshing successful work.
(app/'proof-check').unlink()
(app/'build.mcpp').write_text(build_program(slow=True))
with (work/'cancel.log').open('w') as log:
 process=subprocess.Popen([env['MCPP'],'build'],cwd=app,env=env,stdout=log,stderr=log,start_new_session=True)
 deadline=time.monotonic()+40
 while not (app/'proof-check').exists():
  assert process.poll() is None,(work/'cancel.log').read_text()
  assert time.monotonic()<deadline,'cancel worker did not start'
  time.sleep(.1)
 channel=Path((app/'proof-check').read_text())
 os.killpg(process.pid,signal.SIGINT)
 assert process.wait(timeout=15)!=0,'cancelled build succeeded'
 assert not channel.exists(),'cancelled execution channel remained'
print('PASS: optional action progress preserves all role outputs and command results')
PY
