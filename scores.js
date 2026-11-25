/* Top-ten ranking, kept in the browser's storage. No DOM access, so the Node
 * tests can run it. `storage` is localStorage, or null to keep scores in memory. */
function createScoreboard(storage) {
  const SIZE = 10, NAME_MAX = 8;
  const KEY = 'exploding-potatoes.top10.v1', NAME_KEY = 'exploding-potatoes.name';
  let memory = [], name = '';

  const cleanName = text =>
    String(text).replace(/\s+/g, ' ').trim().toUpperCase().slice(0, NAME_MAX).trim();

  // stored data may be missing, damaged or hand-edited
  const clean = list => (Array.isArray(list) ? list : [])
    .filter(e => e && typeof e.name === 'string' && Number.isSafeInteger(e.score) && e.score > 0)
    .map(e => ({ name: cleanName(e.name) || 'FARMER', score: e.score }))
    .sort((a, b) => b.score - a.score)           // stable: earlier entries win ties
    .slice(0, SIZE);

  function list() {
    try {
      const text = storage.getItem(KEY);
      if (text !== null) memory = clean(JSON.parse(text));
    } catch { /* no storage, or unreadable: use what we have */ }
    return memory.slice();
  }

  const qualifies = score => {
    const top = list();
    return score > 0 && (top.length < SIZE || score > top[SIZE - 1].score);
  };

  /* Returns the new entry's position (0 = first), or -1 if it did not place. */
  function add(who, score) {
    if (!qualifies(score)) return -1;
    const top = list();                          // re-read: another tab may have saved
    name = cleanName(who);
    const entry = { name: name || 'FARMER', score };
    let at = top.findIndex(e => e.score < score);
    if (at < 0) at = top.length;
    top.splice(at, 0, entry);
    memory = top.slice(0, SIZE);
    try {
      storage.setItem(KEY, JSON.stringify(memory));
      storage.setItem(NAME_KEY, name);
    } catch { storage = null; /* full or blocked: remember for this visit */ }
    return at;
  }

  function lastName() {
    try { return cleanName(storage.getItem(NAME_KEY) || name); } catch { return name; }
  }

  return { size: SIZE, list, qualifies, add, lastName };
}
if (typeof module === 'object') module.exports = createScoreboard;
