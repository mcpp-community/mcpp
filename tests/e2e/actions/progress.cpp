// Action progress: an optional execution channel that preserves every role's outputs and
// results, and its paths on every host (formerly tests/e2e/1013, 1015). The action workers are
// Python programs and the first case needs a terminal and a process group, so each case hands its
// script's Python driver to python3, unchanged.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view preserves = R"py(from pathlib import Path
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
)py";

constexpr std::string_view portable = R"py(from pathlib import Path
import json,os,subprocess,sys,tempfile
with tempfile.TemporaryDirectory(prefix='mcpp-progress-') as temporary:
    root=Path(temporary)/'项目-进度';(root/'src').mkdir(parents=True)
    (root/'mcpp.toml').write_text('[package]\nname="portable-progress"\nversion="0.1.0"\n',encoding='utf-8')
    (root/'src/main.cpp').write_text('int main(){return 0;}\n',encoding='utf-8')
    worker=root/'worker.py';proof=root/'target/proof.json'
    worker.write_text('''from pathlib import Path
import json,os,sys,time
file=os.environ.get('MCPP_ACTION_PROGRESS_FILE','')
assert file, 'the enabled action has no execution channel'
p=Path(file)
for seq in range(2):
    state=dict(v=1,seq=seq,phase='compile',done=seq,total=1)
    staged=p.with_suffix('.tmp');staged.write_text(json.dumps(state),encoding='utf-8')
    os.replace(staged,p);time.sleep(.25)
Path(sys.argv[1]).write_text(json.dumps(dict(channel=file,snapshot=state)),encoding='utf-8')
''',encoding='utf-8')
    quote=lambda value:json.dumps(str(value),ensure_ascii=False)
    (root/'build.mcpp').write_text('''import std;
import mcpp;
int main(){
    mcpp::action action;
    action.id="portable-worker";action.role=mcpp::roles::check;action.progress=true;
    const auto stamp=std::string(mcpp::out_dir())+"/worker.stamp";
    action.arg(PYTHON).arg(WORKER).arg(PROOF).input(WORKER).output(stamp.c_str());
    action.submit();
}
'''.replace('PYTHON',quote(sys.executable)).replace('WORKER',quote(worker)).replace('PROOF',quote(proof)),encoding='utf-8')
    exe=os.environ['MCPP_PROGRESS_TEST_EXE']
    result=subprocess.run([exe,'build'],cwd=root,env=dict(os.environ,MCPP_PROGRESS='plain'),capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
    assert result.returncode==0,result.stdout+result.stderr
    result=json.loads(proof.read_text(encoding='utf-8'))
    assert '项目-进度' in result['channel'],result
    assert result['snapshot']==dict(v=1,seq=1,phase='compile',done=1,total=1),result
    assert not Path(result['channel']).exists(),'execution channel was not cleaned'
print('PASS: native action progress paths are UTF-8 and cleaned')
)py";

// python3 running `driver` (written to the case's directory) with `env`.
e2e::outcome python(std::string_view driver, std::vector<std::pair<std::string, std::optional<std::string>>> env) {
    const auto python3 = s::on_path("python3");
    expect(fatal(python3.has_value())) << "python3 is not on PATH";
    const auto file = s::case_dir() / "driver.py";
    s::project{file.parent_path(), {}}.write(file.filename().string(), driver);
    return e2e::run({.argv = {python3->string(), file.string()}, .env = std::move(env), .timeout = std::chrono::minutes{10}});
}

E2E progress{"progress", [] {
    "1013 optional action progress preserves outputs"_test[s::gcc][s::unix_shell][s::python3] = [] {
        // An action that reports progress through its execution channel, for every role: the
        // channel is shown through a terminal (plain and verbose) and removed after; turning it
        // off leaves the outputs usable; a failure after a complete count is a failure; a
        // cancelled build removes its channel.
        const auto work = s::case_dir() / "work";
        fs::create_directories(work);
        auto r = python(preserves, {{"MCPP", s::mcpp_path()}, {"MCPP_HOME", s::case_home(s::home::inherited).string()}, {"PROGRESS_TEST_WORK", work.string()}});
        expect(e2e::succeeded(r)) << "the progress driver failed";
        expect(e2e::contains(r.out, "PASS: optional action progress preserves all role outputs and command results")) << "the driver did not finish";
    };

    "1015 action progress paths are utf8 on every host"_test[s::python3] = [] {
        // The worker, environment and file transport of action progress in a non-ASCII project
        // directory, without a terminal or signals, so every host runs it.
        auto r = python(portable, {{"MCPP_PROGRESS_TEST_EXE", fs::path(s::mcpp_path()).make_preferred().string()}});
        expect(e2e::succeeded(r)) << "the progress driver failed";
        expect(e2e::contains(r.out, "PASS: native action progress paths are UTF-8 and cleaned")) << "the driver did not finish";
    };
}};

}  // namespace
