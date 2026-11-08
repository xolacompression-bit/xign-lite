# Changes

## 1.0.0 (first public release)
- Windows, Linux and Android; one archive format for all of them.
- Pack files and folders; restore everything or one file; list; check every file.
- JPEG, PNG and document files (PDF, DOCX, XLSX, PPTX, ODT, EPUB) are opened and stored in a form that packs better,
  and are restored to exactly the same bytes; each one is checked before it is kept. `--no-open` stores them as they are.
- Files that are exact copies of an earlier file are stored once.
- Several pictures and documents are packed and restored at the same time on all processor cores.
- Every restored file is checked; a damaged file is named and never written; the other files are still restored.
- Restoring never overwrites a file in place: it writes a temporary file and renames it only after the check passes.
- Android app: opening pictures and documents and storing copies once come in its next version.
