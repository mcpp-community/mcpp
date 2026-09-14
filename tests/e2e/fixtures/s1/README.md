# S1 build database schema (vendored)

`s1-build-database.schema.json` is the JSON Schema of S1, "C++ Build Database:
IDE Profile", profile version 0.2.0, copied without modification from
`specs/schema/s1-build-database.schema.json` of
https://github.com/Sunrisepeak/lsp-mcpp-private at commit
`b82859d993f21746c73334e1c9da980c92dc6a3f` (the file is unchanged since commit
`28ecd6e3bf440884a63bddaf8d57d2aeb8d64b75`). The specification and its schema are
licensed under the Apache License 2.0.

The file is vendored so that mcpp's tests validate what `mcpp emit
build-database` prints against a fixed version of the specification, without
running or reading anything else from that repository. A new profile version
replaces the file and this record together.

`tests/e2e/_json_schema_subset.py` validates a document against it. The validator
implements the keywords this schema uses and refuses any other, so a schema that
starts using a keyword the validator does not implement fails instead of
passing unchecked.
