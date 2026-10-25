package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const (
	defaultEchoSymbolsBaseURL = "https://raw.githubusercontent.com/Dualgame/echo-symbols"
	defaultSourceName         = "subsets/echo_vr-pc.json"
	defaultQuestSourceName    = "subsets/echo_vr-quest.json"
	maxCorpusBytes            = 64 << 20
)

var (
	shaPattern          = regexp.MustCompile(`^[0-9a-f]{40}$`)
	generatedCountMatch = regexp.MustCompile(`kSymCacheCount = (\d+);`)
)

// ReadPin reads the echo-symbols commit sha from tools/symbols.pin. Blank lines
// and whole-line comments are ignored; anything else must be a full sha.
func ReadPin(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if !shaPattern.MatchString(line) {
			return "", fmt.Errorf("pin %q is not a 40-character lowercase sha", line)
		}
		return line, nil
	}
	return "", fmt.Errorf("pin file %s holds no sha", path)
}

// ReadGeneratedCount reads kSymCacheCount from a generated symcache_data.cpp.
func ReadGeneratedCount(path string) (int, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	match := generatedCountMatch.FindSubmatch(data)
	if match == nil {
		return 0, fmt.Errorf("%s has no kSymCacheCount", path)
	}
	return strconv.Atoi(string(match[1]))
}

// CorpusSource is one input to the merged cache: a local path or, when Path
// is empty, a file fetched at the pin from base/<pin>/Name.
type CorpusSource struct {
	Path string
	Name string
}

// RegenerateOptions describes one regeneration.
type RegenerateOptions struct {
	Pin              string
	PinFile          string
	InputPath        string
	Sources          []CorpusSource
	BaseURL          string
	SourceName       string
	OutPath          string
	PreviousCount    int
	AllowCountChange bool
	IfPinChanged     bool
	HTTPClient       *http.Client
}

// RegenerateResult reports what one regeneration did.
type RegenerateResult struct {
	OutPath   string
	Pin       string
	Count     int
	Previous  int
	Delta     int
	Conflicts int
	Skipped   bool
}

// Regenerate loads the corpus at the given pin (local file or the pinned
// raw.githubusercontent URL), validates it, and writes the C++ table.
func Regenerate(ctx context.Context, options RegenerateOptions) (RegenerateResult, error) {
	result := RegenerateResult{OutPath: options.OutPath, Previous: options.PreviousCount, Pin: options.Pin}
	if result.OutPath == "" {
		result.OutPath = "src/runtime/log/symcache_data.cpp"
	}

	pin := options.Pin
	if pin == "" && options.PinFile != "" {
		read, err := ReadPin(options.PinFile)
		if err != nil {
			return result, err
		}
		pin = read
	}
	if !shaPattern.MatchString(pin) {
		return result, fmt.Errorf("pin %q is not a 40-character lowercase sha", pin)
	}
	result.Pin = pin

	if options.IfPinChanged {
		if existing, err := os.ReadFile(result.OutPath); err == nil && strings.Contains(string(existing), " @ "+pin+"\n") {
			count, err := ReadGeneratedCount(result.OutPath)
			if err != nil {
				return result, err
			}
			result.Skipped = true
			result.Count = count
			return result, nil
		}
	}

	sources := options.corpusSources()
	corpora := make([]Corpus, 0, len(sources))
	names := make([]string, 0, len(sources))
	for _, source := range sources {
		if source.Name == "" {
			source.Name = defaultSourceName
		}
		names = append(names, source.Name)
		corpus, err := loadSource(ctx, options.HTTPClient, options.BaseURL, pin, source)
		if err != nil {
			return result, err
		}
		// Validate each subset on its own: a conflicting Quest name that the
		// merge drops must still be a real symbol of the pinned commit.
		if err := ValidateCorpus(corpus, ValidateOptions{}); err != nil {
			return result, fmt.Errorf("%s at %s: %w", source.Name, pin, err)
		}
		corpora = append(corpora, corpus)
	}

	corpus := corpora[0]
	conflicts := 0
	for _, extra := range corpora[1:] {
		merged, count, err := MergeCorpora(corpus, extra)
		if err != nil {
			return result, err
		}
		corpus = merged
		conflicts += count
	}
	result.Conflicts = conflicts

	validate := ValidateOptions{PreviousCount: options.PreviousCount}
	if options.AllowCountChange {
		validate.PreviousCount = 0
	}
	if err := ValidateCorpus(corpus, validate); err != nil {
		return result, err
	}

	if err := writeFileAtomically(result.OutPath, func(w io.Writer) error {
		return WriteGenerated(w, corpus, strings.Join(names, " + "), pin)
	}); err != nil {
		return result, err
	}

	result.Count = len(corpus)
	result.Delta = len(corpus) - options.PreviousCount
	return result, nil
}

func fetchPinned(ctx context.Context, client *http.Client, baseURL, pin, source string) ([]byte, error) {
	if baseURL == "" {
		baseURL = defaultEchoSymbolsBaseURL
	}
	url := strings.TrimRight(baseURL, "/") + "/" + pin + "/" + strings.TrimLeft(source, "/")
	if client == nil {
		client = &http.Client{Timeout: 60 * time.Second}
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	response, err := client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("fetch %s: %w", url, err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("fetch %s: HTTP %d", url, response.StatusCode)
	}
	data, err := io.ReadAll(io.LimitReader(response.Body, maxCorpusBytes))
	if err != nil {
		return nil, fmt.Errorf("fetch %s: %w", url, err)
	}
	return data, nil
}

func writeFileAtomically(path string, write func(io.Writer) error) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	temp, err := os.CreateTemp(filepath.Dir(path), filepath.Base(path)+".tmp*")
	if err != nil {
		return err
	}
	tempName := temp.Name()
	defer os.Remove(tempName)
	if err := write(temp); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(tempName, 0o644); err != nil {
		return err
	}
	return os.Rename(tempName, path)
}

var errNoPrevious = errors.New("no previous count")

// corpusSources is the ordered input list: explicit Sources, else the legacy
// single InputPath, else the PC and Quest subsets fetched at the pin.
func (options RegenerateOptions) corpusSources() []CorpusSource {
	if len(options.Sources) > 0 {
		return options.Sources
	}
	if options.InputPath != "" {
		name := options.SourceName
		if name == "" {
			name = defaultSourceName
		}
		return []CorpusSource{{Path: options.InputPath, Name: name}}
	}
	return []CorpusSource{
		{Name: defaultSourceName},
		{Name: defaultQuestSourceName},
	}
}

func loadSource(ctx context.Context, client *http.Client, baseURL, pin string, source CorpusSource) (Corpus, error) {
	var data []byte
	if source.Path != "" {
		read, err := os.ReadFile(source.Path)
		if err != nil {
			return nil, err
		}
		data = read
	} else {
		fetched, err := fetchPinned(ctx, client, baseURL, pin, source.Name)
		if err != nil {
			return nil, err
		}
		data = fetched
	}
	corpus, err := LoadCorpus(strings.NewReader(string(data)))
	if err != nil {
		return nil, fmt.Errorf("%s at %s: %w", source.Name, pin, err)
	}
	return corpus, nil
}
