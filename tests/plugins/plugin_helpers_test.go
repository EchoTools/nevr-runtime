package plugins

import (
	"os"
)

const (
	envBuildDir     = "NEVR_BUILD_DIR"
	defaultBuildDir = "../../dist"

	envGameDir     = "EVR_GAME_DIR"
	defaultGameDir = "../../ready-at-dawn-echo-arena"

	envReconstructionDir     = "ECHOVR_RECONSTRUCTION_DIR"
	defaultReconstructionDir = "../../../echovr-reconstruction"

	envEvrReconstructionDir     = "EVR_RECONSTRUCTION_DIR"
	defaultEvrReconstructionDir = "../../../evr-reconstruction"

	envPluginsSourceDir     = "NEVR_PLUGINS_SOURCE_DIR"
	defaultPluginsSourceDir = "../../../nevr-runtime-plugins"
)

func getBuildDir() string {
	if dir := os.Getenv(envBuildDir); dir != "" {
		return dir
	}
	return defaultBuildDir
}

func getGameDir() string {
	if dir := os.Getenv(envGameDir); dir != "" {
		return dir
	}
	return defaultGameDir
}

func getReconstructionDir() string {
	if dir := os.Getenv(envReconstructionDir); dir != "" {
		return dir
	}
	return defaultReconstructionDir
}

func getEvrReconstructionDir() string {
	if dir := os.Getenv(envEvrReconstructionDir); dir != "" {
		return dir
	}
	return defaultEvrReconstructionDir
}

func getPluginsSourceDir() string {
	if dir := os.Getenv(envPluginsSourceDir); dir != "" {
		return dir
	}
	return defaultPluginsSourceDir
}
