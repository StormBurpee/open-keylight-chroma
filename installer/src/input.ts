export type EditKey = {leftArrow?: boolean; rightArrow?: boolean; backspace?: boolean; delete?: boolean; ctrl?: boolean; meta?: boolean};
export function edit(value: string, cursor: number, input: string, key: EditKey): {value: string; cursor: number} {
  const characters = Array.from(value), position = Math.max(0, Math.min(cursor, characters.length));
  if (key.ctrl && input === 'u') return {value: '', cursor: 0};
  if (key.leftArrow) return {value, cursor: Math.max(0, position - 1)};
  if (key.rightArrow) return {value, cursor: Math.min(characters.length, position + 1)};
  if (key.delete) {characters.splice(position, 1); return {value: characters.join(''), cursor: position};}
  if (key.backspace) {
    if (position) characters.splice(position - 1, 1);
    return {value: characters.join(''), cursor: Math.max(0, position - 1)};
  }
  if (key.ctrl || key.meta || /[\x00-\x1f\x7f-\x9f]/.test(input)) return {value, cursor: position};
  const added = Array.from(input);
  if (characters.length + added.length > 1000) return {value, cursor: position};
  characters.splice(position, 0, ...added);
  return {value: characters.join(''), cursor: position + added.length};
}
