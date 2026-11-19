DO $$
DECLARE result jsonb; scenario text; chunk_log integer; batch_size integer; count_chunks integer;
BEGIN
  PERFORM snapshot_seed(0, 7);
  batch_size := (snapshot_capture(1, 'none')::jsonb->>'batch_size')::int;
  count_chunks := batch_size * 2 + 1;
  -- Every supported chunk width, including bitmaps that end in partial bytes.
  FOR chunk_log IN 0..7 LOOP
    PERFORM snapshot_seed(count_chunks, chunk_log);
    result := snapshot_capture(count_chunks, 'none')::jsonb;
    ASSERT (result->>'chunks')::int = count_chunks AND (result->>'unique')::int = count_chunks;
    ASSERT (result->>'ordered')::boolean;
    ASSERT (result->>'allocations_locked')::int = 0;
    ASSERT (result->>'locks')::int = 4; -- count plus three bounded batches
    result := snapshot_capture(17, 'none')::jsonb;
    ASSERT (result->>'chunks')::int = 17 AND (result->>'unique')::int = 17;
    ASSERT (result->>'ordered')::boolean;
    ASSERT snapshot_capture(0, 'none')::jsonb = 'null'::jsonb;
  END LOOP;
  PERFORM snapshot_seed(0, 7);
  result := snapshot_capture(1025, 'none')::jsonb;
  ASSERT (result->>'chunks')::int = 0 AND (result->>'pages')::int = 0;
  PERFORM snapshot_seed(4096, 7);
  result := snapshot_capture(4096, 'empty')::jsonb;
  ASSERT (result->>'chunks')::int = 0 AND (result->>'pages')::int = 0;

  count_chunks := greatest(4096, batch_size * 4);
  FOREACH scenario IN ARRAY ARRAY['pin', 'evict', 'pin_all', 'touch', 'reset', 'disable'] LOOP
    PERFORM snapshot_seed(count_chunks, 7);
    result := snapshot_capture(count_chunks, scenario)::jsonb;
    ASSERT (result->>'chunks')::int BETWEEN batch_size AND count_chunks;
    ASSERT (result->>'allocations_locked')::int = 0;
    IF scenario IN ('pin', 'evict') THEN
      ASSERT (result->>'unique')::int = count_chunks - 1; -- resume using the next lookahead key
    ELSIF scenario IN ('pin_all', 'reset', 'disable') THEN
      ASSERT (result->>'chunks')::int = batch_size; -- valid partial manifest
    ELSIF scenario = 'touch' THEN
      ASSERT (result->>'unique')::int >= count_chunks - batch_size; -- one batch may repeat
    END IF;
    RAISE NOTICE '%: %', scenario, result;
  END LOOP;

  PERFORM snapshot_seed(65536, 7);
  result := snapshot_capture(65536, 'churn')::jsonb;
  ASSERT (result->>'unique')::int >= 58982; -- >=90% with deterministic concurrent churn
  ASSERT (result->>'chunks')::int <= 65536;
  RAISE NOTICE 'churn: %', result;

  PERFORM snapshot_seed(4096, 7);
  BEGIN
    PERFORM snapshot_capture(4096, 'cancel');
    RAISE EXCEPTION 'snapshot ignored cancellation';
  EXCEPTION WHEN query_canceled THEN
    NULL;
  END;
  result := snapshot_capture(4096, 'none')::jsonb;
  ASSERT (result->>'chunks')::int = 4096; -- cancellation released the lock

  PERFORM snapshot_seed(4096, 7, false);
  BEGIN
    PERFORM snapshot_capture(4096, 'none');
    RAISE EXCEPTION 'snapshot accepted invalid tags';
  EXCEPTION WHEN internal_error THEN
    ASSERT position('invalid buffer tag' in SQLERRM) > 0;
  END;
  PERFORM snapshot_seed(4096, 7);
  result := snapshot_capture(4096, 'none')::jsonb;
  ASSERT (result->>'chunks')::int = 4096; -- validation errors release all locks
  RAISE NOTICE 'snapshot regression checks passed';
END
$$;
