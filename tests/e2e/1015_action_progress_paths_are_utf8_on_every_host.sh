#!/usr/bin/env bash
# requires: python3
# Native worker/environment/file transport, including a non-ASCII project
# directory. This avoids pty and Unix signals so Windows runs it too.
set -e
MCPP="${MCPP:-mcpp}"
if command -v cygpath >/dev/null 2>&1; then
    export MCPP_PROGRESS_TEST_EXE="$(cygpath -w "$(command -v "$MCPP")")"
else
    export MCPP_PROGRESS_TEST_EXE="$(command -v "$MCPP")"
fi
python3 - <<'PY'
from pathlib import Path
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
PY
