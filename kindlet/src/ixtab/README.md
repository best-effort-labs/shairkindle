# ixtab Kindlet jailbreak frontend

The Java files in this directory are the established frontend from ixtab's
`kindlejailbreak`, imported for compatibility with the jailbreak gateway already
installed on the target Kindle.

- Author: ixtab
- Upstream discussion and source release:
  <https://www.mobileread.com/forums/showthread.php?t=163358>
- License: WTFPL, as stated in each source file
- Role here: runtime API for requesting permissions for this Kindlet's Java
  protection domain; this directory is not a newly developed jailbreak or
  installation mechanism

Application-specific lifecycle and permission ownership are implemented outside
this imported package in
`com/besteffortlabs/kindletshell/RuntimePermissions.java`.

Preserve these files as attributed upstream code. Record any future local
compatibility or security change in this file.
