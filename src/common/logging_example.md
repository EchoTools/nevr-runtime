# JSON Logging Format

The logging system now outputs structured JSON logs similar to zap.Logger format.

## Example Output

```json
{"level":"info","ts":"2026-02-09T23:10:07.123Z","msg":"[NEVR.PATCH] Initializing GamePatches v1.0.0 (abc5734)"}
{"level":"debug","ts":"2026-02-09T23:10:07.456Z","msg":"[NEVR.PATCH] Service override [serverdb]: https://example.com"}
{"level":"warn","ts":"2026-02-09T23:10:08.789Z","msg":"[NEVR.PATCH] Failed to load custom config file"}
{"level":"error","ts":"2026-02-09T23:10:09.012Z","msg":"Connection failed: timeout"}
```

## Field Descriptions

- `level`: Log level (debug, info, warn, error)
- `ts`: ISO8601 timestamp in UTC with millisecond precision
- `msg`: The formatted log message
- `caller`: (optional) Function/file that generated the log

## Comparison to Previous Format

### Before (ANSI colored text)
```
[NEVR.PATCH] Initializing GamePatches v1.0.0 (abc5734)
[NEVR.PATCH] Service override [serverdb]: https://example.com
```

### After (JSON structured)
```json
{"level":"info","ts":"2026-02-09T23:10:07.123Z","msg":"[NEVR.PATCH] Initializing GamePatches v1.0.0 (abc5734)"}
{"level":"debug","ts":"2026-02-09T23:10:07.456Z","msg":"[NEVR.PATCH] Service override [serverdb]: https://example.com"}
```

## Benefits

1. **Machine-readable**: Easy to parse and process by log aggregation tools
2. **Timestamped**: Every log has precise timestamp with millisecond accuracy
3. **Structured**: Consistent format across all log entries
4. **Filterable**: Easy to filter by level, timestamp, or message content
5. **Compatible**: Works with standard log processing tools (Logstash, Fluentd, etc.)
