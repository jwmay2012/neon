#!/usr/bin/env zsh
setopt errexit nounset pipefail

# Use a disposable PostgreSQL 16 server, with real shared hash tables/LWLocks.
typeset pg_config_bin=${1:-pg_config}
typeset fixture_chunks=${2:-65536}
typeset test_mode=${3:-test}
typeset test_dir=${0:A:h}
typeset neon_dir=${test_dir:h:h}
typeset snapshot_source=${LFC_SNAPSHOT_SOURCE:-$neon_dir/file_cache.c}
typeset scratch
scratch=$(mktemp -d "${TMPDIR:-/tmp}/neon-lfc-state.XXXXXX")
typeset pg_bin=$($pg_config_bin --bindir)
typeset pg_include=$($pg_config_bin --includedir-server)
typeset compiler=$($pg_config_bin --cc)
typeset -a cppflags=( ${(z)$($pg_config_bin --cppflags)} )
typeset -a linkflags=(-shared)
[[ $(uname -s) != Darwin ]] || linkflags=(-bundle -bundle_loader "$pg_bin/postgres")

function cleanup {
  if [[ -f $scratch/data/postmaster.pid ]]; then
    "$pg_bin/pg_ctl" -D "$scratch/data" -m fast -w stop > /dev/null
  fi
}
trap cleanup EXIT

# Extract declarations and the production function, never a hand-copied clone.
awk '/^BufferTagIsValid\(/ { print "bool"; on=1 } on { print } on && /^}/ { exit }' \
  "$neon_dir/neon_utils.c" > "$scratch/validate.inc"
awk '/^typedef enum FileCacheBlockState/ { on=1 } on { print } /^#define SET_STATE/ { on=0 }' \
  "$snapshot_source" > "$scratch/entry.inc"
awk '/^#define FILE_CACHE_STATE_MAGIC/ { on=1 } on { print } /^#define FILE_CACHE_STATE_SIZE\(fcs\)/ { on=0 }' \
  "$snapshot_source" > "$scratch/format.inc"
awk '/^#define LFC_STATE_BATCH_SIZE/ { on=1 } /^FileCacheState\*/ { on=1 } on { print } on && /^}/ { if (function_body) exit } /^lfc_get_state\(/ { function_body=1 }' \
  "$snapshot_source" > "$scratch/snapshot.inc"
[[ -s $scratch/entry.inc && -s $scratch/format.inc && -s $scratch/snapshot.inc ]]

"$compiler" -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  "${cppflags[@]}" -I"$pg_include" -I"$neon_dir" -I"$scratch" \
  -fPIC "${linkflags[@]}" "$test_dir/snapshot_test.c" -o "$scratch/snapshot_test.so"
"$pg_bin/initdb" -D "$scratch/data" --no-locale -A trust > "$scratch/initdb.log"
mkdir "$scratch/socket"
cat >> "$scratch/data/postgresql.conf" <<EOF
shared_preload_libraries = '$scratch/snapshot_test.so'
snapshot_test.capacity = $fixture_chunks
listen_addresses = ''
unix_socket_directories = '$scratch/socket'
max_connections = 10
shared_buffers = '16MB'
EOF
"$pg_bin/pg_ctl" -D "$scratch/data" -l "$scratch/postgres.log" -w start > /dev/null
typeset -a sql=("$pg_bin/psql" -XAtq -v ON_ERROR_STOP=1 -h "$scratch/socket" -d postgres)
"${sql[@]}" <<EOF
CREATE FUNCTION snapshot_seed(integer, integer, boolean DEFAULT true) RETURNS void AS '$scratch/snapshot_test.so', 'snapshot_seed' LANGUAGE C STRICT;
CREATE FUNCTION snapshot_capture(integer, text) RETURNS text AS '$scratch/snapshot_test.so', 'snapshot_capture' LANGUAGE C STRICT;
CREATE FUNCTION snapshot_probe(integer) RETURNS text AS '$scratch/snapshot_test.so', 'snapshot_probe' LANGUAGE C STRICT;
CREATE FUNCTION snapshot_shuffle() RETURNS void AS '$scratch/snapshot_test.so', 'snapshot_shuffle' LANGUAGE C;
EOF
if [[ $test_mode == benchmark ]]; then
  "${sql[@]}" -c "SELECT snapshot_seed($fixture_chunks, 7)" > /dev/null
  [[ ${LFC_SNAPSHOT_SHUFFLE:-false} != true ]] || "${sql[@]}" -c 'SELECT snapshot_shuffle()' > /dev/null
  "${sql[@]}" -c 'SELECT snapshot_probe(15)' > "$scratch/probe.json" &
  typeset probe_pid=$!
  sleep 1
  "${sql[@]}" -c "SELECT snapshot_capture($fixture_chunks, 'none')" | tee "$scratch/capture.json"
  wait $probe_pid
  cat "$scratch/probe.json"
else
  "${sql[@]}" -f "$test_dir/test.sql" | tee "$scratch/results.log"
fi
print -r -- "Evidence: $scratch"
