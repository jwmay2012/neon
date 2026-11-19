DO $$
DECLARE result jsonb; scenario text; chunk_log integer;
BEGIN
  -- Every supported chunk width, including bitmaps that end in partial bytes.
  FOR chunk_log IN 0..7 LOOP
    PERFORM snapshot_seed(1025, chunk_log);
    result := snapshot_capture(1025, 'none')::jsonb;
    ASSERT (result->>'chunks')::int = 1025 AND (result->>'unique')::int = 1025;
    ASSERT (result->>'ordered')::boolean;
    ASSERT (result->>'allocations_locked')::int = 0;
    ASSERT (result->>'locks')::int = 10; -- count plus nine bounded batches
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

  FOREACH scenario IN ARRAY ARRAY['pin', 'evict', 'pin_all', 'touch', 'reset', 'disable'] LOOP
    PERFORM snapshot_seed(4096, 7);
    result := snapshot_capture(4096, scenario)::jsonb;
    ASSERT (result->>'chunks')::int BETWEEN 128 AND 4096;
    ASSERT (result->>'allocations_locked')::int = 0;
    IF scenario IN ('pin', 'evict') THEN
      ASSERT (result->>'unique')::int = 4095; -- resume using the next lookahead key
    ELSIF scenario IN ('pin_all', 'reset', 'disable') THEN
      ASSERT (result->>'chunks')::int = 128; -- valid partial manifest
    ELSIF scenario = 'touch' THEN
      ASSERT (result->>'unique')::int >= 3968; -- a moved key can repeat one batch
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
