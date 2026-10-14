// Package forkdb provides Go bindings for the forkdb CLI tool.
package forkdb

import (
	"encoding/json"
	"fmt"
	"os/exec"
	"path/filepath"
)

// ForkDB represents a forkdb database instance.
type ForkDB struct {
	dbPath  string
	cliPath string
}

// Options for the ForkDB instance.
type Options struct {
	BranchName string
}

// Info represents branch information.
type Info struct {
	Name           string `json:"name"`
	Kind           string `json:"kind"`
	ReadOnly       bool   `json:"readOnly"`
	UID            string `json:"uid"`
	OriginUID      string `json:"originUid"`
	BaseUID        string `json:"baseUid"`
	OwnerUID       string `json:"ownerUid"`
	State          int64  `json:"state"`
	SizePages      int    `json:"sizePages"`
	NUndo          int    `json:"nUndo"`
	OpenConnections int   `json:"openConnections"`
	ID             int    `json:"id"`
}

// List of branches.
type BranchList []Info

// PromoteResult represents the result of a promote operation.
type PromoteResult struct {
	Method       string `json:"method"`
	DryRun       bool   `json:"dryRun"`
	UndoRecorded bool   `json:"undoRecorded"`
	Refusal      string `json:"refusal,omitempty"`
}

// GCReport represents the result of garbage collection.
type GCReport struct {
	OrphanBases    int `json:"orphanBases"`
	OrphanUndo     int `json:"orphanUndo"`
	UnlistedUndo   int `json:"unlistedUndo"`
	OverRetention int `json:"overRetention"`
	Total          int `json:"total"`
}

// CompactResult represents the result of a compact operation.
type CompactResult struct {
	Compacted bool `json:"compacted"`
}

// New creates a new ForkDB instance.
func New(dbPath string) (*ForkDB, error) {
	cliPath, err := findCLI()
	if err != nil {
		return nil, err
	}

	return &ForkDB{
		dbPath:  filepath.Clean(dbPath),
		cliPath: cliPath,
	}, nil
}

// findCLI locates the forkdb CLI executable.
func findCLI() (string, error) {
	// Look for the CLI in expected locations
	possiblePaths := []string{
		"../../../build/forkdb",
		"../../../cli/forkdb",
		"forkdb", // Try system PATH
	}

	for _, cliPath := range possiblePaths {
		if _, err := exec.LookPath(cliPath); err == nil {
			return cliPath, nil
		}
	}

	return "", fmt.Errorf("forkdb CLI not found. Please build it first with ./scripts/build-step4.sh cli")
}

// Init initializes a new database.
func (f *ForkDB) Init(branchName string) error {
	args := []string{f.dbPath, "init", "--name", branchName}
	return f.executeCommand(args, nil)
}

// Fork creates a new branch from an existing branch.
func (f *ForkDB) Fork(from, name string) error {
	args := []string{f.dbPath, "fork", name, "--from", from}
	return f.executeCommand(args, nil)
}

// Snapshot creates a read-only snapshot from a branch.
func (f *ForkDB) Snapshot(from, name string) error {
	args := []string{f.dbPath, "snapshot", name, "--from", from}
	return f.executeCommand(args, nil)
}

// Discard removes a branch or snapshot.
func (f *ForkDB) Discard(name string) error {
	args := []string{f.dbPath, "discard", name}
	return f.executeCommand(args, nil)
}

// Reset resets a branch to match another branch's content.
func (f *ForkDB) Reset(name, to string) error {
	args := []string{f.dbPath, "reset", name, to}
	return f.executeCommand(args, nil)
}

// Info gets information about a branch.
func (f *ForkDB) Info(ref string) (*Info, error) {
	args := []string{f.dbPath, "info", ref, "--json"}
	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var info Info
	if err := json.Unmarshal([]byte(output), &info); err != nil {
		return nil, fmt.Errorf("failed to parse info output: %w", err)
	}

	return &info, nil
}

// List lists all branches.
func (f *ForkDB) List(includeInternal bool) (*BranchList, error) {
	args := []string{f.dbPath, "list"}
	if includeInternal {
		args = append(args, "--all")
	}
	args = append(args, "--json")

	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var list BranchList
	if err := json.Unmarshal([]byte(output), &list); err != nil {
		return nil, fmt.Errorf("failed to parse list output: %w", err)
	}

	return &list, nil
}

// Promote promotes a branch to its target.
func (f *ForkDB) Promote(name string, into string, dryRun bool) (*PromoteResult, error) {
	args := []string{f.dbPath, "promote", name}
	
	if into != "" {
		args = append(args, "--into", into)
	}
	
	if dryRun {
		args = append(args, "--dry-run")
	}
	
	args = append(args, "--json")

	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var result PromoteResult
	if err := json.Unmarshal([]byte(output), &result); err != nil {
		return nil, fmt.Errorf("failed to parse promote output: %w", err)
	}

	return &result, nil
}

// Undo undoes the last promotion into a branch.
func (f *ForkDB) Undo(name string) (*PromoteResult, error) {
	args := []string{f.dbPath, "undo", name, "--json"}
	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var result PromoteResult
	if err := json.Unmarshal([]byte(output), &result); err != nil {
		return nil, fmt.Errorf("failed to parse undo output: %w", err)
	}

	return &result, nil
}

// GC performs garbage collection.
func (f *ForkDB) GC() (*GCReport, error) {
	args := []string{f.dbPath, "gc", "--json"}
	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var report GCReport
	if err := json.Unmarshal([]byte(output), &report); err != nil {
		return nil, fmt.Errorf("failed to parse gc output: %w", err)
	}

	return &report, nil
}

// Compact compacts the database if needed.
func (f *ForkDB) Compact(minBytes int64) (*CompactResult, error) {
	args := []string{f.dbPath, "compact", "--min-bytes", fmt.Sprintf("%d", minBytes), "--json"}
	output, err := f.executeCommandWithOutput(args)
	if err != nil {
		return nil, err
	}

	var result CompactResult
	if err := json.Unmarshal([]byte(output), &result); err != nil {
		return nil, fmt.Errorf("failed to parse compact output: %w", err)
	}

	return &result, nil
}

// SQL executes SQL on a branch.
func (f *ForkDB) SQL(ref, sqlQuery string, write bool) (string, error) {
	args := []string{f.dbPath, "sql", ref}
	if write {
		args = append(args, "--write")
	}
	args = append(args, sqlQuery)

	return f.executeCommandWithOutput(args)
}

// executeCommand executes a forkdb CLI command.
func (f *ForkDB) executeCommand(args []string, output interface{}) error {
	cmd := exec.Command(f.cliPath, args...)
	if output == nil {
		cmd.Stdout = nil
		cmd.Stderr = nil
	}
	
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("forkdb command failed: %w", err)
	}
	
	return nil
}

// executeCommandWithOutput executes a forkdb CLI command and returns its output.
func (f *ForkDB) executeCommandWithOutput(args []string) (string, error) {
	cmd := exec.Command(f.cliPath, args...)
	output, err := cmd.Output()
	if err != nil {
		if exitError, ok := err.(*exec.ExitError); ok {
			return "", fmt.Errorf("forkdb command failed with code %d: %s", exitError.ExitCode(), string(exitError.Stderr))
		}
		return "", fmt.Errorf("forkdb command failed: %w", err)
	}
	
	return string(output), nil
}

// Open creates and initializes a database.
func Open(dbPath, branchName string) (*ForkDB, error) {
	db, err := New(dbPath)
	if err != nil {
		return nil, err
	}
	
	if err := db.Init(branchName); err != nil {
		return nil, err
	}
	
	return db, nil
}