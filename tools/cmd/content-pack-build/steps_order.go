package main

import (
	"fmt"
	"slices"
	"strings"
)

// orderedWith returns list in an order that satisfies every After and Before: Kahn's algorithm, and among the steps ready
// at once it takes the one that tie puts first (tie(a, b) true: a before b, a and b being registration indexes). It refuses,
// with one error naming the steps: a name registered twice, a declared name no step has, a step that names itself, and a
// cycle.
func orderedWith(list []Step, tie func(a, b int) bool) ([]Step, error) {
	index := make(map[string]int, len(list))
	for i, st := range list {
		if _, dup := index[st.Name]; dup {
			return nil, fmt.Errorf("step %q is registered twice", st.Name)
		}
		index[st.Name] = i
	}
	preds := make([][]int, len(list)) // preds[i]: the steps that run before step i
	for i, st := range list {
		for _, n := range st.After {
			j, err := declaredIndex(index, st.Name, n, "after")
			if err != nil {
				return nil, err
			}
			preds[i] = append(preds[i], j)
		}
		for _, n := range st.Before {
			j, err := declaredIndex(index, st.Name, n, "before")
			if err != nil {
				return nil, err
			}
			preds[j] = append(preds[j], i)
		}
		for _, n := range st.OrderFree {
			if _, err := declaredIndex(index, st.Name, n, "order-free with"); err != nil {
				return nil, err
			}
		}
	}
	left := make([]int, len(list)) // unplaced predecessors of each step
	succ := make([][]int, len(list))
	for i, ps := range preds {
		left[i] = len(ps)
		for _, p := range ps {
			succ[p] = append(succ[p], i)
		}
	}
	done := make([]bool, len(list))
	out := make([]Step, 0, len(list))
	for len(out) < len(list) {
		best, found := 0, false
		for i := range list {
			if done[i] || left[i] > 0 {
				continue
			}
			if !found || tie(i, best) {
				best, found = i, true
			}
		}
		if !found {
			break
		}
		done[best] = true
		out = append(out, list[best])
		for _, s := range succ[best] {
			left[s]--
		}
	}
	if len(out) < len(list) {
		var stuck []string
		for i, st := range list {
			if !done[i] {
				stuck = append(stuck, fmt.Sprintf("%q", st.Name))
			}
		}
		return nil, fmt.Errorf("cycle among %s", strings.Join(stuck, ", "))
	}
	return out, nil
}

// declaredIndex is the registration index of the step a declaration names, or the refusal of a declaration that names
// nothing or itself.
func declaredIndex(index map[string]int, self, named, kind string) (int, error) {
	j, ok := index[named]
	if !ok {
		return 0, fmt.Errorf("step %q is %s %q, which names no registered step", self, kind, named)
	}
	if named == self {
		return 0, fmt.Errorf("step %q names itself (%s)", self, kind)
	}
	return j, nil
}

// ordered is orderedWith with the lowest registration index first. Registration index is the init order of the files (the
// Go tool sorts files by name); it decides only between steps that no declaration and no shared record relates.
func ordered(list []Step) ([]Step, error) {
	return orderedWith(list, func(a, b int) bool { return a < b })
}

// productionSteps is ordered(steps), the list run, the oracle and the stock tests use.
func productionSteps() ([]Step, error) {
	return ordered(steps)
}

// recordKey is one record of one client set and type, which is what two steps can both write.
type recordKey struct {
	set    string
	typ    uint64
	record uint64
}

// indexPair is two registration indexes, the first the earlier step.
type indexPair struct{ first, second int }

// reachable[i][j] is true when step j runs after step i in the transitive closure of the After and Before declarations.
// A declaration that names no step is skipped here; orderedWith refuses it.
func reachable(list []Step) [][]bool {
	index := make(map[string]int, len(list))
	for i, st := range list {
		index[st.Name] = i
	}
	reach := make([][]bool, len(list))
	for i := range reach {
		reach[i] = make([]bool, len(list))
	}
	for i, st := range list {
		for _, a := range st.After {
			if j, ok := index[a]; ok {
				reach[j][i] = true
			}
		}
		for _, b := range st.Before {
			if j, ok := index[b]; ok {
				reach[i][j] = true
			}
		}
	}
	for k := range list {
		for i := range list {
			if !reach[i][k] {
				continue
			}
			for j := range list {
				if reach[k][j] {
					reach[i][j] = true
				}
			}
		}
	}
	return reach
}

// unorderedPairs is each pair of distinct steps whose Writes name one (set, type, record) that is neither related by the
// transitive closure of After/Before (either direction) nor named by either one's OrderFree, as "name | name" lines, sorted.
func unorderedPairs(list []Step) []string {
	reach := reachable(list)
	writers := map[recordKey][]int{} // the steps writing each record, in registration order
	for i, st := range list {
		for _, f := range st.Writes {
			for _, r := range f.Records {
				k := recordKey{set: f.Set, typ: f.Type, record: r}
				if !slices.Contains(writers[k], i) {
					writers[k] = append(writers[k], i)
				}
			}
		}
	}
	pairs := map[indexPair]bool{}
	for _, ws := range writers {
		for x := range ws {
			for y := x + 1; y < len(ws); y++ {
				pairs[indexPair{first: ws[x], second: ws[y]}] = true
			}
		}
	}
	var out []string
	for p := range pairs {
		a, b := list[p.first], list[p.second]
		if reach[p.first][p.second] || reach[p.second][p.first] || slices.Contains(a.OrderFree, b.Name) || slices.Contains(b.OrderFree, a.Name) {
			continue
		}
		out = append(out, a.Name+" | "+b.Name)
	}
	slices.Sort(out)
	return out
}
