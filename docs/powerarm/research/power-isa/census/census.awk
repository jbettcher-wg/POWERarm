# Static A64 mnemonic/shape census over llvm-objdump -d --no-show-raw-insn output.
# Input lines of interest look like:  "<addr>: <tab>mnemonic<tab>operands"  or "<tab>mnemonic<tab>operands".
BEGIN { FS = "\t" }
{
  # strip a leading "addr:" column if present
  line = $0
  n = split(line, f, "\t")
  if (n < 2) next
  m = f[2]; ops = (n >= 3) ? f[3] : ""
  sub(/^ +/, "", m); sub(/ +$/, "", m)
  if (m == "" || m ~ /^</ || m ~ /^\.|^[0-9a-f]+:$/) next
  total++
  # ---------------- shifts / bitfield ----------------
  if (m == "asr")  { if (ops ~ /#/) c["asr_imm"]++; else c["asr_reg"]++ }
  if (m == "sbfx") c["sbfx"]++
  if (m == "lsr" && ops ~ /#/) c["lsr_imm"]++
  if (m == "lsl" && ops ~ /#/) c["lsl_imm"]++
  if (m == "ror") c["ror"]++
  if (m == "extr") c["extr"]++
  if (m == "ubfx" || m == "ubfiz" || m == "bfi" || m == "bfxil" || m == "sbfiz") c["bitfield_other"]++
  if (m == "sxtw") c["sxtw"]++
  # ---------------- flags ----------------
  if (m == "tst")  c["tst"]++
  if (m == "ands") c["ands"]++
  if (m == "ccmp" || m == "ccmn") c["ccmp"]++
  if (m == "cmp" || m == "cmn") c["cmp"]++
  if (m == "adds" || m == "subs" || m == "negs") c["adds_subs"]++
  if (m == "adc" || m == "adcs" || m == "sbc" || m == "sbcs" || m == "ngc" || m == "ngcs") c["adc_sbc"]++
  if (m ~ /^b\./) c["b_cond"]++
  if (m == "cbz" || m == "cbnz") c["cbz"]++
  if (m == "tbz" || m == "tbnz") c["tbz"]++
  if (m == "csel") c["csel"]++
  if (m == "cset" || m == "csetm") c["cset"]++
  if (m == "csinc" || m == "csinv" || m == "csneg" || m == "cinc" || m == "cinv" || m == "cneg") c["csinc_inv_neg"]++
  # ---------------- bit ops ----------------
  if (m == "rbit") c["rbit"]++
  if (m == "clz")  c["clz"]++
  if (m == "cls")  c["cls"]++
  if (m == "rev")  { if (ops ~ /^x/) c["rev64"]++; else c["rev32"]++ }
  if (m == "rev16" || m == "rev32") c["rev16_32"]++
  # ---------------- multiply / divide ----------------
  if (m == "madd") c["madd"]++
  if (m == "msub") c["msub"]++
  if (m == "smaddl" || m == "umaddl" || m == "smsubl" || m == "umsubl") c["smaddl_umaddl"]++
  if (m == "smull" || m == "umull") { if (ops ~ /\./) c["vmull_vec"]++; else c["smull_umull"]++ }
  if (m == "mul" && ops !~ /\./) c["mul"]++
  if (m == "mneg") c["mneg"]++
  if (m == "smulh" || m == "umulh") c["mulh"]++
  if (m == "sdiv") c["sdiv"]++
  if (m == "udiv") c["udiv"]++
  # ---------------- loads / stores ----------------
  if (m ~ /^(ldr|str|ldrb|strb|ldrh|strh|ldrsb|ldrsh|ldrsw|ldur|stur)$/) {
    if (ops ~ /, [wx][0-9]+, sxtw/)      c["ls_regoff_sxtw"]++
    else if (ops ~ /, [wx][0-9]+, uxtw/) c["ls_regoff_uxtw"]++
    else if (ops ~ /, x[0-9]+, lsl/)     c["ls_regoff_lsl"]++
    else if (ops ~ /, x[0-9]+\]/)        c["ls_regoff_plain"]++
    else if (ops ~ /\]!/)                c["ls_preindex"]++
    else if (ops ~ /\], #/)              c["ls_postindex"]++
    else                                 c["ls_imm"]++
    if (ops ~ /^[dsqhb][0-9]+,/)         c["ls_fpr"]++
  }
  if (m == "ldp" || m == "stp") {
    if (ops ~ /\]!/) c["ldp_stp_preindex"]++
    else if (ops ~ /\], #/) c["ldp_stp_postindex"]++
    else c["ldp_stp_imm"]++
  }
  if (m ~ /^(ldar|ldarb|ldarh|ldapr|ldaprb|ldaprh|stlr|stlrb|stlrh)$/) c["acq_rel_ls"]++
  if (m ~ /^(ldaxr|ldxr|stxr|stlxr|ldaxrb|ldaxrh|ldxrb|ldxrh|stlxrb|stlxrh|stxrb|stxrh|ldaxp|ldxp|stlxp|stxp)$/) c["exclusives"]++
  if (m ~ /^(cas|casa|casl|casal|casb|casab|caslb|casalb|cash|casah|caslh|casalh|casp|caspa|caspl|caspal)$/) c["lse_cas"]++
  if (m ~ /^(ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|swp)/) c["lse_rmw"]++
  if (m == "dmb" || m == "dsb" || m == "isb") c["barriers"]++
  # ---------------- calls ----------------
  if (m == "bl") c["bl"]++
  if (m == "blr") c["blr"]++
  if (m == "ret") c["ret"]++
  if (m == "br") c["br"]++
  # ---------------- scalar FP ----------------
  if (m ~ /^(fadd|fsub|fmul|fdiv|fnmul|fmadd|fmsub|fnmadd|fnmsub|fsqrt|fabs|fneg|fmax|fmin|fmaxnm|fminnm)$/ && ops !~ /\./) c["fp_scalar_arith"]++
  if (m == "fcmp" || m == "fcmpe") c["fcmp"]++
  if (m == "fccmp" || m == "fccmpe") c["fccmp"]++
  if (m == "fcsel") c["fcsel"]++
  if (m == "fmov") {
    if (ops ~ /^d[0-9]+, x/) c["fmov_d_from_x"]++
    else if (ops ~ /^s[0-9]+, w/) c["fmov_s_from_w"]++
    else if (ops ~ /^[xw][0-9]+, [ds]/ || ops ~ /^x[0-9]+, v/) c["fmov_gpr_from_fpr"]++
    else if (ops ~ /#/) c["fmov_imm"]++
    else c["fmov_other"]++
  }
  if (m ~ /^(scvtf|ucvtf)$/) { if (ops ~ /\./) c["cvt_to_float_vec"]++; else c["cvt_to_float_scalar"]++ }
  if (m ~ /^(fcvtzs|fcvtzu|fcvtns|fcvtnu|fcvtas|fcvtau|fcvtms|fcvtmu|fcvtps|fcvtpu)$/) { if (ops ~ /\./) c["cvt_to_int_vec"]++; else c["cvt_to_int_scalar"]++ }
  if (m == "fcvt") c["fcvt_scalar"]++
  if (m ~ /^(fcvtl|fcvtl2|fcvtn|fcvtn2|fcvtxn)$/) c["fcvt_vec_width"]++
  if (m ~ /^(frinta|frinti|frintm|frintn|frintp|frintx|frintz)$/) c["frint"]++
  # ---------------- NEON ----------------
  if (m == "addp") { if (ops ~ /\./) c["addp_vec"]++; else c["addp_scalar"]++ }
  if (m == "umaxp" || m == "uminp") c["umaxp_uminp"]++
  if (m == "addv" || m == "uaddlv" || m == "saddlv" || m == "umaxv" || m == "uminv" || m == "smaxv" || m == "sminv") c["vec_reduce"]++
  if (m == "cmeq" || m == "cmhi" || m == "cmhs" || m == "cmgt" || m == "cmge" || m == "cmtst" || m == "cmlt" || m == "cmle") c["vec_cmp"]++
  if (m == "dup") c["dup"]++
  if (m == "ins" || m == "umov" || m == "smov" || m == "mov" && ops ~ /^v[0-9]+\./) c["ins_umov"]++
  if (m == "ld1r" || m == "ld2r" || m == "ld3r" || m == "ld4r") c["ld1r"]++
  if (m ~ /^(ld1|ld2|ld3|ld4|st1|st2|st3|st4)$/) c["ldN_stN"]++
  if (m == "tbl" || m == "tbx") c["tbl"]++
  if (m ~ /^(zip1|zip2|uzp1|uzp2|trn1|trn2|ext)$/) c["zip_uzp_trn_ext"]++
  if (m ~ /^(shrn|shrn2|rshrn|rshrn2|xtn|xtn2|sqxtn|sqxtn2|uqxtn|uqxtn2|sqxtun|sqxtun2)$/) c["narrow"]++
  if (m ~ /^(sshll|sshll2|ushll|ushll2|sxtl|sxtl2|uxtl|uxtl2)$/) c["widen"]++
  if (m ~ /^(shl|sshr|ushr|sli|sri|srshr|urshr|ssra|usra|sshl|ushl|srshl|urshl|sqshl|uqshl)$/ && ops ~ /\./) c["vec_shift"]++
  if (m ~ /^(fadd|fsub|fmul|fdiv|fmla|fmls|fmax|fmin|fmaxnm|fminnm|fabs|fneg|fsqrt|fcmeq|fcmgt|fcmge|fcmlt|fcmle|fmulx|fabd)$/ && ops ~ /\./) c["fp_vec_arith"]++
  if (m ~ /^(add|sub|mul|and|orr|eor|bic|orn|not|mvn|neg|abs|smax|smin|umax|umin|sabd|uabd|saba|uaba|mla|mls)$/ && ops ~ /^v[0-9]+\./) c["int_vec_arith"]++
  if (m ~ /^(aese|aesd|aesmc|aesimc|sha1|sha256|pmull|pmull2|crc32)/) c["crypto_crc"]++
  if (m ~ /^(sdot|udot|sqrdmlah|sqrdmlsh|sqdmulh|sqrdmulh)/) c["dotprod_rdm"]++
  if (m ~ /^(movi|mvni|fmov)$/ && ops ~ /^v[0-9]+\./) c["vec_imm"]++
  if (m == "mrs") c["mrs"]++
  if (m == "nop") c["nop"]++
  if (m == "adrp") c["adrp"]++
  if (m == "movz" || m == "movk" || m == "movn" || (m == "mov" && ops ~ /#/)) c["movwide"]++
}
END {
  printf "total\t%d\n", total
  for (k in c) printf "%s\t%d\t%.3f%%\n", k, c[k], 100.0 * c[k] / total
}
