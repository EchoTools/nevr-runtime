package main

import (
	"fmt"
	"strings"
)

// MergeCorpora unions the PC and Quest subsets. A hash present in both keeps
// the PC name: if the names differ only by case (the CSymbol64 hash folds
// case), that is a reported conflict; if they differ beyond case they cannot
// both be the same symbol, so the merge fails.
func MergeCorpora(pc, quest Corpus) (Corpus, int, error) {
	merged := make(Corpus, len(pc)+len(quest))
	for hash, name := range pc {
		merged[hash] = name
	}
	conflicts := 0
	for hash, questName := range quest {
		pcName, exists := merged[hash]
		if !exists {
			merged[hash] = questName
			continue
		}
		if pcName == questName {
			continue
		}
		if strings.EqualFold(pcName, questName) {
			conflicts++
			continue
		}
		return nil, conflicts, fmt.Errorf("conflict: hash %#016x is %q in the PC subset and %q in the Quest subset; names that differ beyond case cannot be one symbol", hash, pcName, questName)
	}
	return merged, conflicts, nil
}
