"""
forkdb - Python wrapper for the forkdb CLI tool
"""

import subprocess
import json
import os
from typing import List, Dict, Any, Optional

class ForkDB:
    """
    A Python wrapper for the forkdb CLI tool.
    
    This class provides a convenient interface to interact with forkdb
    repositories without needing to call the CLI directly.
    """
    
    def __init__(self, db_path: str):
        """
        Initialize a ForkDB instance.
        
        Args:
            db_path (str): Path to the database file
        """
        self.db_path = os.path.abspath(db_path)
        self.cli_path = self._find_cli()
    
    def _find_cli(self) -> str:
        """
        Find the forkdb CLI executable.
        
        Returns:
            str: Path to the forkdb CLI executable
            
        Raises:
            FileNotFoundError: If forkdb CLI is not found
        """
        # Look for the CLI in expected locations
        possible_paths = [
            os.path.join(os.path.dirname(__file__), '..', '..', '..', 'build', 'forkdb'),
            os.path.join(os.path.dirname(__file__), '..', '..', '..', 'cli', 'forkdb'),
            'forkdb'  # Try system PATH
        ]
        
        for cli_path in possible_paths:
            if os.path.exists(cli_path):
                return cli_path
                
        raise FileNotFoundError(
            "forkdb CLI not found. Please build it first with ./scripts/build-step4.sh cli"
        )
    
    def _execute_command(self, args: List[str]) -> str:
        """
        Execute a forkdb CLI command.
        
        Args:
            args (List[str]): Command arguments
            
        Returns:
            str: Command output
            
        Raises:
            subprocess.CalledProcessError: If the command fails
        """
        cmd = [self.cli_path, self.db_path] + args
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout.strip()
        except subprocess.CalledProcessError as e:
            raise RuntimeError(
                f"forkdb command failed with code {e.returncode}: {e.stderr}"
            )
    
    def init(self, branch_name: str = "main") -> 'ForkDB':
        """
        Initialize a new database.
        
        Args:
            branch_name (str): Name of the initial branch (default: 'main')
            
        Returns:
            ForkDB: This instance
        """
        self._execute_command(["init", "--name", branch_name])
        return self
    
    def fork(self, from_branch: str, name: str) -> 'ForkDB':
        """
        Create a new branch from an existing branch.
        
        Args:
            from_branch (str): Source branch name
            name (str): New branch name
            
        Returns:
            ForkDB: This instance
        """
        self._execute_command(["fork", name, "--from", from_branch])
        return self
    
    def snapshot(self, from_branch: str, name: str) -> 'ForkDB':
        """
        Create a read-only snapshot from a branch.
        
        Args:
            from_branch (str): Source branch name
            name (str): Snapshot name
            
        Returns:
            ForkDB: This instance
        """
        self._execute_command(["snapshot", name, "--from", from_branch])
        return self
    
    def discard(self, name: str) -> 'ForkDB':
        """
        Remove a branch or snapshot.
        
        Args:
            name (str): Branch or snapshot name to discard
            
        Returns:
            ForkDB: This instance
        """
        self._execute_command(["discard", name])
        return self
    
    def reset(self, name: str, to: str) -> 'ForkDB':
        """
        Reset a branch to match another branch's content.
        
        Args:
            name (str): Branch to reset
            to (str): Source branch to reset to
            
        Returns:
            ForkDB: This instance
        """
        self._execute_command(["reset", name, to])
        return self
    
    def info(self, ref: str = "main") -> Dict[str, Any]:
        """
        Get information about a branch.
        
        Args:
            ref (str): Branch reference (default: 'main')
            
        Returns:
            Dict[str, Any]: Branch information
        """
        output = self._execute_command(["info", ref, "--json"])
        return json.loads(output)
    
    def list(self, include_internal: bool = False) -> List[Dict[str, Any]]:
        """
        List all branches.
        
        Args:
            include_internal (bool): Include internal branches (default: False)
            
        Returns:
            List[Dict[str, Any]]: Array of branch information
        """
        args = ["list"]
        if include_internal:
            args.append("--all")
        args.append("--json")
        
        output = self._execute_command(args)
        return json.loads(output)
    
    def promote(self, name: str, into: Optional[str] = None, dry_run: bool = False) -> Dict[str, Any]:
        """
        Promote a branch to its target.
        
        Args:
            name (str): Branch name to promote
            into (str, optional): Target branch (default: branch's origin)
            dry_run (bool): Run without making changes (default: False)
            
        Returns:
            Dict[str, Any]: Promotion result
        """
        args = ["promote", name]
        
        if into:
            args.extend(["--into", into])
        
        if dry_run:
            args.append("--dry-run")
        
        args.append("--json")
        
        output = self._execute_command(args)
        return json.loads(output)
    
    def undo(self, name: str) -> Dict[str, Any]:
        """
        Undo the last promotion into a branch.
        
        Args:
            name (str): Branch name to undo
            
        Returns:
            Dict[str, Any]: Undo result
        """
        output = self._execute_command(["undo", name, "--json"])
        return json.loads(output)
    
    def gc(self) -> Dict[str, Any]:
        """
        Perform garbage collection.
        
        Returns:
            Dict[str, Any]: GC report
        """
        output = self._execute_command(["gc", "--json"])
        return json.loads(output)
    
    def compact(self, min_bytes: int = 0) -> Dict[str, Any]:
        """
        Compact the database if needed.
        
        Args:
            min_bytes (int): Minimum bytes threshold (default: 0)
            
        Returns:
            Dict[str, Any]: Compaction result
        """
        output = self._execute_command([
            "compact", "--min-bytes", str(min_bytes), "--json"
        ])
        return json.loads(output)
    
    def sql(self, ref: str, sql_query: str, write: bool = False) -> str:
        """
        Execute SQL on a branch.
        
        Args:
            ref (str): Branch reference
            sql_query (str): SQL query to execute
            write (bool): Allow write operations (default: False)
            
        Returns:
            str: Query result
        """
        args = ["sql", ref]
        if write:
            args.append("--write")
        args.append(sql_query)
        
        return self._execute_command(args)


def open_forkdb(db_path: str, branch_name: str = "main") -> ForkDB:
    """
    Convenience function to create and initialize a database.
    
    Args:
        db_path (str): Path to the database file
        branch_name (str): Initial branch name (default: 'main')
        
    Returns:
        ForkDB: Initialized ForkDB instance
    """
    db = ForkDB(db_path)
    db.init(branch_name)
    return db