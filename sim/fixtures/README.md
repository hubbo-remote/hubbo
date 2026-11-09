# hub-sim fixtures

Intact but unusable config documents, for putting hub-sim into the Blocked
state (API contract runs, web e2e). Write one as the next generation of the
`config` record, then start the sim on the same directory:

    hub-sim write-record --data DIR --name config --file sim/fixtures/config.newer-schema.json
    hub-sim serve --data DIR

- `config.newer-schema.json`: schema_version 99 (a downgrade): Blocked, reason unsupported_schema.
- `config.does-not-compile.json`: references an unregistered device: Blocked, reason validation_failed.

`serve --reset` (or POST /sim/reset) restores the seed and leaves Blocked.
