#!/usr/bin/env python3
"""Recover completed module objects in one actual build, before Ninja reads BMIs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mcpp',type=Path)
    parser.add_argument('project',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    mcpp=args.mcpp.resolve();project=args.project.resolve();output=args.output.resolve()
    rows=json.loads((output/'compile_commands.json').read_text())
    objects=[Path(row['output']) for row in rows if row['file'].endswith(('src/core.cppm','src/mid.cppm'))]
    assert len(objects)==2,'two actual module objects are required'
    def build(success=True):
        result=subprocess.run([str(mcpp),'build','-j2'],cwd=project,capture_output=True,text=True,timeout=60)
        assert (result.returncode==0)==success,result.stdout+result.stderr
        return result.stdout+result.stderr
    def identity():
        return {str(p.relative_to(output)):(p.stat().st_mtime_ns,hashlib.sha256(p.read_bytes()).hexdigest())
                for pattern in ('obj/**/*.o','gcm.cache/**/*.gcm') for p in output.glob(pattern)}
    graph=(output/'build.ninja').read_bytes()
    for missing in (objects[:1],objects):
        for path in missing:path.unlink()
        build()
        assert all(path.is_file() for path in objects),'one invocation did not restore missing objects'
        before=identity();build();assert identity()==before,'recovery changed the following no-op'
        assert (output/'build.ninja').read_bytes()==graph,'recovery replaced its build graph'
        print(f'PASS: {len(missing)} missing completed objects recover in one build; following no-op unchanged',flush=True)
    certificate=Path(str(objects[0])+'.sched.completed');original=certificate.read_bytes()
    bmi_before={p:(p.stat().st_mtime_ns,p.read_bytes()) for p in (output/'gcm.cache').rglob('*.gcm')}
    try:
        certificate.write_bytes(b'X'+original[1:]);objects[0].unlink()
        build(False)
        assert all(p.exists() and (p.stat().st_mtime_ns,p.read_bytes())==value for p,value in bmi_before.items()),'corrupt proof authorized BMI deletion'
        assert not objects[0].exists(),'Ninja ran despite corrupt completion evidence'
    finally:
        certificate.write_bytes(original)
    build();assert objects[0].exists()
    print('PASS: corrupt completion evidence refuses admission without deleting any BMI',flush=True)
    outside=project/'unrelated-output';outside.mkdir();marker=outside/'unrelated.gcm';marker.write_text('unrelated')
    alias=output/'obj/unrelated-cache-alias'
    alias.symlink_to(outside,target_is_directory=True)
    try:
        log=build(False)
        assert 'symlinked cached compiler state' in log,log
        assert marker.read_text()=='unrelated','directory symlink authorized outside output modification'
    finally:
        alias.unlink()
    build();before=identity();build();assert identity()==before
    print('PASS: directory symlink refuses admission and preserves unrelated output',flush=True)


if __name__=='__main__':main()
