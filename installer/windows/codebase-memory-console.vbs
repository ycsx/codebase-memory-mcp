Option Explicit

Dim appPath
Dim command
Dim fileSystem
Dim shell

Set fileSystem = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
appPath = fileSystem.BuildPath(fileSystem.GetParentFolderName(WScript.ScriptFullName), _
                               "codebase-memory-mcp.exe")
command = """" & appPath & """ console"
shell.Run command, 0, False
