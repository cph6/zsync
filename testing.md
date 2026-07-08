# Testing Information

## Requirements

Before running integration tests:
- Apache HTTP server installed. It does not have to be
  running - the test suite runs a copy with a custom config.
- OpenSSL installed
- tinyproxy (for proxy test)

## Running tests

```
go test ./...                     # Run all tests
go test -v -run TestZSyncMakeSimple ./t/zsync # Run one test, show output
go test -short                       # Unit tests only
LARGE_TESTS=yes go test              # All integration tests.

## History

The start-to-finish zsync tests in ./t and syncer_full_test.go had existed for a long time in
a separate repository, as they used non-free data. As part
of the golang port, I switched the tests to use generated
data, converted them to golang and merged them into the
main repository.
```
