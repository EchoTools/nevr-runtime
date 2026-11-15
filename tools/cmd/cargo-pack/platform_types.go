package main

import (
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/csymbol"
)

// A record's on-disk type names its platform. The engine makes the type from the resource's class and the build's platform word:
// CSymbol64(<class>) continued over <platform word> + <subtype suffix>. The Quest build's word is "Android" (libr15.so
// CResource::ComputeAssetType 0x2641a0c copies "Android", appends kAssetSubTypeSuffixes[subtype] and calls CSymbol64::Append); the
// PC build's is "Win10" (echovr.exe CResourceID_BuildPath 0x140fa19a0, docs/engine/asset-system.md "Names"). Subtype 1 is "GPU":
// CGTextureResource + "AndroidGPU" and + "Win10GPU" are the payload types of src/cdn/cdn_manifest.h's kAndroidPairs and
// kWindowsPairs. A Quest client looks a record up by its Android type, so a Win10 type under android-arm64 is a record no Quest game
// reads, and the other way round on the PC.

// platformWord is each cargo platform's word (src/cdn/cdn_manifest.h kAndroidArm64, kWindowsX64). It is also the set of platforms
// the tool takes.
var platformWord = map[string]string{"android-arm64": "Android", "windows-x86_64": "Win10"}

// subtypeSuffixes are the engine's kAssetSubTypeSuffixes: variant 0 (the descriptor) and variant 1 (the GPU payload).
var subtypeSuffixes = []string{"", "GPU"}

// resourceClasses are the classes whose on-disk types the tool names. The first four are the classes of src/core/resource_set_registry.h
// and the texture streaming's mip records (asset-system.md); the rest are every class of a type in the combat_content label, each
// found by hashing the class names in src/runtime/log/symcache_data.cpp with "Win10" and "Win10GPU". The one type of that label no
// class name hashes to, 0x06eace9f64204fd0 (the script image), stays unnamed: an unnamed type is not refused.
var resourceClasses = []string{
	"CArchiveResource", "CGameLevelResource", "CScriptResource", "CGTextureResource", "RawTexturePackfile",
	"CActorDataResource", "CBoundingSphereCR", "CCanvasUICR", "CComponentLODCR", "CFrustumCullCR", "CGInstancedModelResource",
	"CGLightMapResource", "CGMaterialResource", "CGMeshListResource", "CGParticleEffectResource", "CGSceneResource",
	"CGShaderSetResource", "CGTextureStreamingResource", "CGVisibilityResource", "CInstanceModelCR", "CJsonResource", "CModelCR",
	"COcclusionCullCR", "CParticleEffectCR", "CPhysicsResource", "CR15ButtonInteractCR", "CR15DampenerCR", "CR15NetBulletCR",
	"CR15NetDamageableCR", "CR15NetExplosionCR", "CR15NetHealBubbleCR", "CR15NetMagazineCR", "CR15ParentedConstraintCR",
	"CR15RigidAttachConstraintCR", "CScriptCR", "CTextureOverrideCR", "CTextureStreamingCR", "CTransformCR", "CUICanvasResource",
	"CWWiseSoundBankResource",
}

// typeOwner is an on-disk type the tool names: the platform it belongs to and the class name and suffixes it is the hash of.
type typeOwner struct {
	platform, name string
}

// knownTypes maps every named on-disk type, written as cargo writes a type ("0x" and 16 hex digits), to its owner. No two names
// hash to one type (TestEveryNamedTypeIsOneClassOfOnePlatform counts them).
var knownTypes = func() map[string]typeOwner {
	m := map[string]typeOwner{}
	for plat, word := range platformWord {
		for _, class := range resourceClasses {
			for _, suffix := range subtypeSuffixes {
				name := class + word + suffix
				m[fmt.Sprintf("0x%0*x", hexDigits, csymbol.Hash(name))] = typeOwner{platform: plat, name: name}
			}
		}
	}
	return m
}()

// checkPlatformType refuses a record whose type is a named on-disk type of another platform. A type the table does not name is
// taken as it is, and reported as unnamed (false).
func checkPlatformType(r record) (named bool, err error) {
	owner, ok := knownTypes[r.typ]
	if !ok {
		return false, nil
	}
	if owner.platform != r.platform {
		return true, fmt.Errorf("%s %s (%s): type %s is %s, an on-disk type of %s, not of %s: no %s game looks it up", r.typ, r.name, r.source,
			r.typ, owner.name, owner.platform, r.platform, r.platform)
	}
	return true, nil
}
