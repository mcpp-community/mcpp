#!/usr/bin/env python3
"""Rewrite an mcpp-generated build.ninja so importers are released at BMI-flush
instead of at compiler exit. Prototype for the "BMI-release scheduling" proposal.

Per module interface unit:

    build obj/X.m.o | gcm.cache/X.gcm : cxx_module src/X.cppm | X.ddi.dd
      dyndep = X.ddi.dd

becomes two edges driven by ONE compiler process:

    build gcm.cache/X.gcm : cxx_module_bmi src/X.cppm | X.ddi.dd   # exits at BMI rename
      dyndep = X.ddi.dd
    build obj/X.m.o       : cxx_module_obj gcm.cache/X.gcm         # waits for codegen

Nothing downstream needs rewriting: dyndep already makes importers depend on
gcm.cache/X.gcm, so they simply become satisfiable ~4x earlier. The link edge
depends on obj/*.m.o and still waits for every object.

The one non-obvious rewrite: dyndep attaches its module deps to whatever the
scan recorded as `-fdeps-target`, which is `obj/X.m.o`. If left alone, the
imports would gate the edge that merely WAITS, while the edge that actually
COMPILES would start with no BMIs present.

Repointing that target needs care, because mcpp's cxx_scan rule spends
`$compile_target` TWICE — once as `-fdeps-target=` and once as `-o`. Changing
the shared variable would aim the preprocessor's `-o` at the BMI path and risk
truncating it. So the rule is rewritten to read `-fdeps-target=$deps_target`,
and only that new variable is repointed; `-o $compile_target` is left alone.

Total CPU work is unchanged: still exactly one `g++ -c` per module.

Limitation (fine for cold-build timing, not for incremental correctness): the
prototype drops the `-MMD` depfile plumbing, so header dependencies of global
module fragments are not tracked.

Usage: split_graph.py <build.ninja> <out.ninja> <proto_dir>
"""
import re, sys, os


def main():
    src, dst, proto = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
    lines = open(src).read().split('\n')

    edge_re = re.compile(r'^build (\S+) \| (\S+) : cxx_module (\S+)(.*)$')
    scan_re = re.compile(r'^build (\S+) : cxx_scan (\S+)(.*)$')

    # pass 1: obj -> gcm, so the scan edges can be repointed
    gcm_of_obj = {}
    for line in lines:
        m = edge_re.match(line)
        if m:
            gcm_of_obj[m.group(1)] = m.group(2)

    rules = f'''rule cxx_module_bmi
  command = {proto}/bmi_release.sh $slot $out -- $cxx $local_includes $cxxflags $unit_cxxflags -x c++ -c $in -o $obj_out
  description = BMI $out
  restat = 1

rule cxx_module_obj
  command = {proto}/bmi_wait.sh $slot $out
  description = OBJ $out
  restat = 1

'''

    out, i, n_split, n_scan = [], 0, 0, 0
    while i < len(lines):
        line = lines[i]

        if line.startswith('rule cxx_object'):
            out.append(rules.rstrip('\n'))
            out.append('')
            out.append(line)
            i += 1
            continue

        # rewrite the scan RULE so -fdeps-target reads its own variable
        if line.strip().startswith('command =') and '-fdeps-format=p1689r5' in line:
            out.append(line.replace('-fdeps-target=$compile_target',
                                    '-fdeps-target=$deps_target'))
            i += 1
            continue

        m = scan_re.match(line)
        if m:
            out.append(line)
            i += 1
            extra = None
            while i < len(lines) and lines[i].startswith('  '):
                p = lines[i]
                key = p.strip().split('=')[0].strip()
                if key == 'compile_target':
                    tgt = p.split('=', 1)[1].strip()
                    # -o keeps pointing at the object; only the dyndep target moves
                    extra = f'  deps_target = {gcm_of_obj.get(tgt, tgt)}'
                    if tgt in gcm_of_obj:
                        n_scan += 1
                out.append(p)
                i += 1
            if extra:
                out.append(extra)
            continue

        m = edge_re.match(line)
        if m:
            obj, gcm, source, tail = m.groups()
            props, j = [], i + 1
            while j < len(lines) and lines[j].startswith('  '):
                props.append(lines[j])
                j += 1
            keep = [p for p in props if not p.strip().startswith('bmi_out')]
            slot = 'slots/' + gcm.replace('/', '_')
            out.append(f'build {gcm} : cxx_module_bmi {source}{tail}')
            out.extend(keep)
            out.append(f'  obj_out = {obj}')
            out.append(f'  slot = {slot}')
            out.append(f'build {obj} : cxx_module_obj {gcm}')
            out.append(f'  slot = {slot}')
            n_split += 1
            i = j
            continue

        out.append(line)
        i += 1

    open(dst, 'w').write('\n'.join(out))
    print(f'split {n_split} module edges, repointed {n_scan} scan targets -> {dst}')


if __name__ == '__main__':
    main()
