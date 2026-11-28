# Sharing an OpenCore config safely

The active `EFI/OC/config.plist` is useful for diagnosing early PCI and ACPI boot stalls. Before sharing it, remove SMBIOS identity values while preserving the actual configuration.

## Windows

1. Copy `EFI/OC/config.plist` from the EFI USB to a normal folder on Windows. Do not edit the live EFI.
2. Open PowerShell in the repository folder (or download the script) and run:

   ```powershell
   py -3 tools/rig/redact-opencore.py "E:\EFI\OC\config.plist"
   ```

   Replace `E:\` with the actual EFI drive letter.
3. The script creates `config.redacted.plist` beside the original. It never overwrites the source.
4. Open the redacted file and check that the configuration entries remain intact. Attach only the redacted copy.

The script replaces `SystemSerialNumber`, `MLB`, and `SystemUUID` with placeholders. It replaces the six-byte `ROM` value with six zero bytes, preserving the plist data type and length. It does not remove or reorder kernel patches, ACPI entries, device properties, boot arguments, or quirks.

## Important

- Keep the original EFI and a known-good recovery copy unchanged.
- Do not share the unredacted config, EFI folder, or any file containing your real SMBIOS identity.
- Redaction is deliberately narrow. Check the output for any other personal information before sharing.
- A redacted config is for static review, not for booting. Keep using your original config on the machine.
