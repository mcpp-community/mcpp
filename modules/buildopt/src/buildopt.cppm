// mcpp.buildopt — what mcpp does beyond what the compiler and ninja do, in one
// package (SPEC-019).
//
//   registry   the register: every entry, its seam, level, default and switch
//   keep       edge seam: which previous BMI or object stays
//   schedule   graph seam: which shape the module edges take
//   source     source seam: which units a source is built as
//   advice     what a build says of the rebuild it made; changes nothing
//
// What is specific to one compiler is a module of its own under the part it
// belongs to (keep.gcm). The executor that holds and replaces files, and the
// graph writer, are the pipeline's and ask here.

export module mcpp.buildopt;

export import mcpp.buildopt.registry;
export import mcpp.buildopt.keep;
export import mcpp.buildopt.schedule;
export import mcpp.buildopt.source;
export import mcpp.buildopt.advice;
