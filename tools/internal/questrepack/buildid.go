package questrepack

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
	"unicode/utf8"
)

// The loader's build-ID rule (src/quest/plugin/plugin_image.h): the notes are read from the PT_NOTE segments of a 64-bit
// little-endian ELF, each name and description padded to 4 bytes.

// noteDesc is the description of the first note owned by owner with noteType in a PT_NOTE segment of image; ok is false
// when there is none or the headers do not hold together.
func noteDesc(image []byte, owner []byte, noteType uint32) ([]byte, bool) {
	if len(image) < elfHeaderBytes || !bytes.HasPrefix(image, []byte("\x7fELF")) || image[4] != elfClass64 || image[5] != elfDataLE {
		return nil, false
	}
	total := uint64(len(image))
	phoff := binary.LittleEndian.Uint64(image[elfPhoffOffset:])
	phentsize := uint64(binary.LittleEndian.Uint16(image[elfPhentsizeOff:]))
	phnum := uint64(binary.LittleEndian.Uint16(image[elfPhnumOff:]))
	if phentsize < elfPhdrMinBytes || phoff > total || phnum*phentsize > total-phoff {
		return nil, false
	}
	for k := uint64(0); k < phnum; k++ {
		ph := image[phoff+k*phentsize:]
		pType := binary.LittleEndian.Uint32(ph[0:])
		pOffset := binary.LittleEndian.Uint64(ph[8:])
		pFilesz := binary.LittleEndian.Uint64(ph[32:])
		if pType != ptNote || pOffset > total || pFilesz > total-pOffset {
			continue
		}
		at, end := pOffset, pOffset+pFilesz
		for end-at >= noteHeaderBytes {
			namesz := uint64(binary.LittleEndian.Uint32(image[at:]))
			descsz := uint64(binary.LittleEndian.Uint32(image[at+4:]))
			nType := binary.LittleEndian.Uint32(image[at+8:])
			at += noteHeaderBytes
			nameLen, descLen := alignNote(namesz), alignNote(descsz)
			if nameLen > end-at {
				break
			}
			name := image[at : at+namesz]
			if nType == noteType && bytes.Equal(name, owner) && descsz <= end-at-nameLen {
				return image[at+nameLen : at+nameLen+descsz], true
			}
			if descLen > end-at-nameLen {
				break
			}
			at += nameLen + descLen
		}
	}
	return nil, false
}

// alignNote rounds a note's name or description length up to the note alignment.
func alignNote(n uint64) uint64 {
	return (n + noteAlign - 1) &^ (noteAlign - 1)
}

// buildIDProblems is why the loader would refuse plugin for its build IDs against this libr15; empty when it would not.
func buildIDProblems(libr15, plugin []byte, pluginName string) []string {
	running, ok := noteDesc(libr15, []byte("GNU\x00"), ntGNUBuildID)
	if !ok || len(running) == 0 {
		return []string{fmt.Sprintf("the store APK's %s has no GNU build ID: nothing can be checked", libr15Path)}
	}
	declared, ok := noteDesc(plugin, []byte("NEVR\x00"), nevrNoteType)
	if !ok {
		return []string{fmt.Sprintf("plugin %s declares no libr15 build IDs (NEVR_DECLARE_SUPPORTED_BUILD_IDS); the loader "+
			"refuses it as build_ids_undeclared", pluginName)}
	}
	list := string(declared)
	if i := strings.IndexByte(list, 0); i >= 0 {
		list = list[:i]
	}
	if !utf8.ValidString(list) {
		list = strings.ToValidUTF8(list, "�")
	}
	ids := strings.FieldsFunc(list, func(r rune) bool { return r == ' ' || r == ',' || r == '\t' || r == '\r' || r == '\n' })
	runningHex := hex.EncodeToString(running)
	if !contains(ids, runningHex) {
		shown := pyList(ids)
		if len(ids) == 0 {
			shown = "an empty list"
		}
		return []string{fmt.Sprintf("plugin %s declares %s, not the store libr15 build ID %s; the loader refuses it as "+
			"build_id_unsupported", pluginName, shown, runningHex)}
	}
	return nil
}

// checkPluginBuildIDs reads the store APK's libr15.so and the plugin, and returns the problems.
func checkPluginBuildIDs(apk, plugin string) ([]string, error) {
	z, err := openZip(apk)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	if !z.has(libr15Path) {
		return []string{fmt.Sprintf("the store APK has no %s", libr15Path)}, nil
	}
	libr15, err := z.readAll(libr15Path)
	if err != nil {
		return nil, err
	}
	pluginBytes, err := os.ReadFile(plugin)
	if err != nil {
		return nil, err
	}
	return buildIDProblems(libr15, pluginBytes, baseName(plugin)), nil
}
