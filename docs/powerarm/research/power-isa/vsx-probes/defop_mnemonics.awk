# For each DEF_OP(Name) body, list distinct emitter mnemonics called.
# A mnemonic call is an identifier followed by '(' that is in the known set
# (built from Emitter.h method names passed via -v names=...).
BEGIN { n = split(names, arr, ","); for (i=1;i<=n;i++) known[arr[i]]=1 }
/^DEF_OP\(/ { if (cur != "") flush(); match($0, /DEF_OP\(([A-Za-z0-9_]+)\)/, m); cur = m[1]; delete seen; next }
/^}/ { if (cur != "") flush(); cur = ""; next }
cur != "" {
  line = $0
  while (match(line, /([A-Za-z_][A-Za-z0-9_]*)\(/, m)) {
    id = m[1]
    if (id in known) seen[id] = 1
    line = substr(line, RSTART + RLENGTH)
  }
}
function flush(   k, out) { out = ""; for (k in seen) out = out " " k; printf "%s:%s%s\n", FILENAME, cur, out }
END { if (cur != "") flush() }
