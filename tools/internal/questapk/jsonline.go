package questapk

import (
	"bytes"
	"encoding/json"
	"strings"
)

// Field is one key of a JSON object written as a log line, in the order the line writes it.
type Field struct {
	Key   string
	Value any
}

// Line is fields as one JSON object on one line, the keys in the order given, with no HTML escaping. It is the form the
// Python tools' json.dumps wrote (the same keys in the same order); a value is encoded by encoding/json, never by hand.
func Line(fields ...Field) (string, error) {
	var b strings.Builder
	b.WriteByte('{')
	for i, f := range fields {
		if i > 0 {
			b.WriteByte(',')
		}
		k, err := encodeValue(f.Key)
		if err != nil {
			return "", err
		}
		v, err := encodeValue(f.Value)
		if err != nil {
			return "", err
		}
		b.Write(k)
		b.WriteByte(':')
		b.Write(v)
	}
	b.WriteByte('}')
	return b.String(), nil
}

// encodeValue is v as compact JSON, without the trailing newline an Encoder writes.
func encodeValue(v any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	return bytes.TrimSuffix(buf.Bytes(), []byte("\n")), nil
}
