import * as vscode from 'vscode';

export interface ByteRange {
  startByte: number;
  endByte: number;
}

function utf8LenOfCodePoint(cp: number): number {
  if (cp <= 0x7f) return 1;
  if (cp <= 0x7ff) return 2;
  if (cp <= 0xffff) return 3;
  return 4;
}

export function utf16OffsetAtUtf8Byte(text: string, targetByte: number): number {
  if (targetByte <= 0) return 0;

  let bytes = 0;
  let i = 0; // UTF-16 index

  while (i < text.length && bytes < targetByte) {
    const cp = text.codePointAt(i);
    if (cp === undefined) break;

    const utf16Units = cp > 0xffff ? 2 : 1;
    const utf8Units = utf8LenOfCodePoint(cp);

    if (bytes + utf8Units > targetByte) {
      break;
    }

    bytes += utf8Units;
    i += utf16Units;
  }

  return i;
}

export function utf8ByteAtUtf16Offset(text: string, targetUtf16Offset: number): number {
  if (targetUtf16Offset <= 0) return 0;

  let bytes = 0;
  let i = 0;

  while (i < text.length && i < targetUtf16Offset) {
    const cp = text.codePointAt(i);
    if (cp === undefined) break;

    const utf16Units = cp > 0xffff ? 2 : 1;
    const utf8Units = utf8LenOfCodePoint(cp);

    // If adding this code point would pass the target offset, stop.
    if (i + utf16Units > targetUtf16Offset) break;

    bytes += utf8Units;
    i += utf16Units;
  }

  return bytes;
}

export function byteRangeToRange(doc: vscode.TextDocument, r: ByteRange): vscode.Range {
  const text = doc.getText();
  const startOff = utf16OffsetAtUtf8Byte(text, r.startByte);
  const endOff = utf16OffsetAtUtf8Byte(text, r.endByte);
  return new vscode.Range(doc.positionAt(startOff), doc.positionAt(endOff));
}

export function byteOffsetAtPosition(doc: vscode.TextDocument, pos: vscode.Position): number {
  const text = doc.getText();
  const off = doc.offsetAt(pos);
  return utf8ByteAtUtf16Offset(text, off);
}
