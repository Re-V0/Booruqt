B.LAIR_QT @VERSION@ for Windows (64-bit), portable

Nothing is installed by this program itself. It needs one thing from
Microsoft that this folder cannot carry by itself: the Visual C++ runtime.


1. Install the Visual C++ runtime (once per computer)

   Run vc_redist.x64.exe from this folder and follow its installer.
   It needs administrator rights.

   This build needs runtime version @RUNTIME_VERSION@ or newer, and
   vc_redist.x64.exe here is exactly that version. A Visual C++ runtime
   that other programs installed earlier may be older; run the installer
   anyway - it does nothing if a newer one is already there.

   Without it, blairqt.exe does not start and Windows names a missing
   VCRUNTIME140.dll or MSVCP140.dll, or the program closes right after
   starting.


2. Start the program

   Run blairqt.exe. The folder can live anywhere, also on a USB drive,
   but keep its contents together: blairqt.exe loads the files and
   subfolders next to it.

   Settings and the library index are kept in your user profile, not in
   this folder. Downloaded images go to the download folder chosen in
   the program (Settings -> Library -> Download folder...).


3. Python plugins (optional)

   Plugin sources need Python 3: the "py" launcher from python.org, or
   python on PATH. Set BLAIRQT_PYTHON to an interpreter's full path to
   use a different one.


Source code and issues: https://gitlab.com/re-v0/blairqt
