# M2: the FP/SIMD gaps in the Arch Linux ARM toolchain

Same method as `M1b-SIMD-SUBSET.md`: every instruction word in the executable
sections is decoded against the frontend's own table (`a64.inc`, in
`DecodeTable.cpp`'s order) by `unittests/A64Frontend/simd_census.py`. This time
the census counts only what the frontend does **not** translate
(`--missing-only`): FP/SIMD entries without a translator in
`IRBuilder::HandlerTable`, the LD2-4/ST2-4 forms that the LD1/ST1 multiple-structure
translator rejects, and any word in the FP/SIMD encoding space that no table
entry matches (none were found). Counts are static occurrences.

## Corpus

The pinned Arch Linux ARM sysroot (`~/.local/share/powerarm/RootFS/ArchLinuxARM-m2`,
GCC 16.1.1, binutils 2.46, glibc 2.43, make 4.4.1), taken as the toolchain
programs and the closure of their `DT_NEEDED` libraries:

`gcc`, `cc1`, `as`, `ld`, `make`, `libc.so.6`, `ld-linux-aarch64.so.1`, `libm.so.6`,
`libisl`, `libmpc`, `libmpfr`, `libgmp`, `libz`, `libzstd`, `libopcodes`, `libbfd`,
`libctf`, `libjansson`, `libguile-3.0`, `libsframe`, `libgc`, `libffi`,
`libunistring`, `libcrypt` (cc1 links libstdc++ statically).

The `minimal-debian` rootfs of the a64diff harness uses the Pi's Debian glibc
2.41 `libc.so.6` and `ld-linux-aarch64.so.1`; they are counted separately at the
end.

To regenerate, on a machine with the sysroot:

```sh
R=~/.local/share/powerarm/RootFS/ArchLinuxARM-m2
unittests/A64Frontend/simd_census.py --missing-only --markdown gcc=$R/usr/bin/gcc cc1=$R/usr/lib/gcc/aarch64-unknown-linux-gnu/16.1.1/cc1 ...
```

## Reachable IFUNC variants

The Arch shared objects are stripped of `.symtab`, so the census cannot attribute
code to glibc's IFUNC variants by name as M1b did. Every occurrence is counted. The
variants the presented Cortex-A76 profile (`fp asimd fphp asimdhp cpuid`, no SVE,
no MOPS) never selects are mostly SVE code, which has no entry in `a64.inc` and
therefore contributes nothing here. The two blockers found by running the
toolchain are both reachable: `tbl` in glibc's `strspn` (`libc.so.6+0xa1ba4`, stopped
`gcc -c`) and `ld1 {v31.d}[1], [x2]` in Debian's rtld (`_dl_mcount` area, stopped
the `minimal-debian` rootfs jobs).

## Headline

| Rank | Class (missing) | Count | Main users |
|---:|---|---:|---|
| 1 | SIMD single-structure load/store (`st1`/`ld1` lane, `ld1r`, `st2`/`st3`/`st4` lane) | 397 | cc1 (344), libisl |
| 2 | SIMD two-register misc (vector `fneg` 190, `uadalp` 20, `uaddlp` 8, vector `scvtf` 2, `rev16` 1) | 221 | libm (177), libgmp, libc |
| 3 | SIMD three same (`ushl` 149, `sshl` 16, `mul` 5, `mls` 2, `smaxp` 2) | 174 | cc1 (127), libzstd, libc |
| 4 | SIMD shift by immediate (`sri` 60, `sli` 15, `usra` 9) | 84 | libzstd, libisl |
| 5 | SIMD three different (`ssubw`/`ssubw2` 38, `umlal`/`umlal2` 28, `umull`/`umull2` 10) | 76 | libc (24), ld (24), cc1 |
| 6 | LD2/LD4/ST2/ST3/ST4 multiple structures | 58 | libz (30), libbfd, libisl, libc |
| 7 | scalar `neg d` | 52 | cc1 |
| 8 | scalar pairwise `addp d, v.2d` | 13 | libc, gcc, cc1 |
| 9 | `tbl` | 9 | libz, libc (`strspn`), ld.so, libisl, libzstd |
| 10 | scalar `fabd` | 3 | libm |
| 11 | `mul` by element | 2 | libisl |
| 12 | scalar `uqsub` | 1 | cc1 |

About 1090 occurrences in total.

## Implementation status

All of it is translated (powerarm-m2/simd-gaps), composed from existing IR ops:

| Class | Entries |
|---|---|
| Structures | LD2/LD3/LD4 and ST2/ST3/ST4 multiple structures; LD1-4/ST1-4 single structures; LD1R-LD4R |
| Table lookup | TBL, TBX with 1-4 tables (ISA 3.0 `vpermr` lowering, POWER8 `vperm`) |
| Shifts | USHL/SSHL (vector and scalar), SRI, SLI, USRA, SSRA |
| Widening | UADDLP, SADDLP, UADALP, SADALP, SSUBW, USUBW, UMULL/SMULL/UMLAL/SMLAL/UMLSL/SMLSL (and 2) |
| Arithmetic | MUL/MLA/MLS vector, MUL by element, SMAXP/SMINP, REV16 |
| Scalar | NEG, ABS, ADDP, UQSUB (with FPSR.QC), FABD |
| Float lanes | FNEG, FABS, FABD, SCVTF, UCVTF (vector) |

Tests: A64Frontend `simd_table` (every index value 0..255 for 1-4 tables),
`simd_struct`, `simd_gaps`, plus `simd_struct1`/`simd_struct2` from powerarm-m2/gcc.
With it, `cc1 -version` and `gcc -O2 -c hello.c` (object byte-identical to the Pi), the link
and the linked program all run on the 64K host and in the 4K guest.

## Full counts

Per-entry counts of what is missing, grouped by class, per binary. "Entry" is the
`a64.inc` decoder entry, "Mnemonic" is objdump's alias.

### Advanced SIMD Load/Store single structures: 397

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| ST1_sngl_1 | st1 | 226 | 0 | 199 | 0 | 0 | 0 | 0 | 0 | 0 | 27 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z11expand_callP9tree_nodeP7rtx_defi@@Base, _Z14mcf_smooth_cfgv@@Base, _Z15vect_do_peelingP14_loop_vec_infoP9tree_nodeS2_PS2_S3_S3_ibbS3_@@Base |
| LD1_sngl_1 | ld1 | 158 | 0 | 145 | 0 | 0 | 0 | 13 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z14mcf_smooth_cfgv@@Base, _Z16ipa_analyze_nodeP11cgraph_node@@Base, _Z17store_constructorP9tree_nodeP7rtx_defi8poly_intILj2ElEb@@Base |
| ST3_sngl_1 | st3 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | ffi_closure_free@@LIBFFI_CLOSURE_8.0, ffi_prep_go_closure@@LIBFFI_GO_CLOSURE_8.0 |
| ST4_sngl_1 | st4 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | ffi_closure_free@@LIBFFI_CLOSURE_8.0, ffi_prep_go_closure@@LIBFFI_GO_CLOSURE_8.0 |
| LD1R_1 | ld1r | 3 | 0 | 0 | 1 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | .text, _IO_fopen@@GLIBC_2.17, __nss_hash@@GLIBC_PRIVATE |
| ST2_sngl_1 | st2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | ffi_closure_free@@LIBFFI_CLOSURE_8.0 |

### SIMD Two-register misc: 221

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FNEG_2 | fneg | 190 | 0 | 0 | 0 | 0 | 0 | 10 | 0 | 175 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 5 | 0 | 0 | 0 | 0 | 0 | __acosf_finite@GLIBC_2.17, __asinf_finite@GLIBC_2.17, __atan2_finite@GLIBC_2.17 |
| UADALP | uadalp | 20 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | __gmpn_hamdist@@Base, __gmpn_popcount@@Base |
| UADDLP | uaddlp | 8 | 2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | __gmpn_hamdist@@Base, __gmpn_popcount@@Base, _cpp_clean_line@@Base |
| SCVTF_int_4 | scvtf | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ufromfpxf@@GLIBC_2.43 |
| REV16_asimd | rev16 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | sframe_get_fre_udata@@LIBSFRAME_3.0-0xda8 |

### SIMD three same: 174

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| USHL_2 | ushl | 149 | 2 | 122 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 16 | ZSTD_ldm_blockCompress@@Base, _Z23cl_optimization_restoreP11gcc_optionsS0_P15cl_optimization@@Base, _Z24cl_target_option_restoreP11gcc_optionsS0_P16cl_target_option@@Base |
| SSHL_2 | sshl | 16 | 0 | 3 | 0 | 0 | 0 | 2 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 | 8 | 0 | 0 | 0 | 0 | 0 | _Z16df_hard_reg_initv@@Base, _Z8init_ggcv@@Base, _dl_mcount@@GLIBC_2.17 |
| MUL_vec | mul | 5 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | __gmp_mt_recalc_buffer@@Base, _obstack_begin@@Base-0x52ce8 |
| MLS_vec | mls | 2 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | decNumberCompare@@Base |
| SMAXP | smaxp | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | isl_basic_map_gist_domain@@Base |

### SIMD Shift by immediate: 84

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SRI_2 | sri | 60 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 60 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ZSTD_compressBlock_fast_extDict@@Base |
| SLI_2 | sli | 15 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 15 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ZSTD_compressBlock_fast_extDict@@Base |
| USRA_2 | usra | 9 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 6 | 0 | 0 | 0 | 0 | 0 | isl_basic_map_eliminate_vars@@Base, isl_basic_map_gist_domain@@Base, isl_basic_set_foreach_bound_pair@@Base |

### SIMD three different: 76

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SSUBW | ssubw2 | 20 | 0 | 6 | 0 | 0 | 0 | 14 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z19sched_split_block_1P15basic_block_defP7rtx_def@@Base, _Z24handle_aligned_attributePP9tree_nodeS0_S0_iPb@@Base, _ZN14token_streamer6streamEP10cpp_readerPK9cpp_tokenm@@Base |
| UMLAL_vec | umlal | 19 | 0 | 0 | 0 | 17 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _obstack_begin@@Base-0x52ce8, isl_basic_map_overlying_set@@Base, isl_mat_alloc@@Base |
| SSUBW | ssubw | 18 | 0 | 8 | 0 | 0 | 0 | 10 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z15preprocess_fileP10cpp_reader@@Base, _Z18sorted_attr_stringP9tree_node@@Base, _Z19sched_split_block_1P15basic_block_defP7rtx_def@@Base |
| UMLAL_vec | umlal2 | 9 | 0 | 0 | 0 | 7 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _obstack_begin@@Base-0x52ce8, isl_basic_map_overlying_set@@Base, isl_mat_alloc@@Base |
| UMULL_vec | umull | 5 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | bfd_merge_sections@@Base, decNumberCompare@@Base |
| UMULL_vec | umull2 | 5 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | bfd_merge_sections@@Base, decNumberCompare@@Base |

### SIMD&FP register load/store: 58

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| LDx_mult_1 | ld2 | 28 | 0 | 2 | 0 | 0 | 0 | 5 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 15 | 0 | 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ZSTD_compressBlock_fast_extDict@@Base, _Z16df_hard_reg_initv@@Base, _Z37aarch64_operands_adjust_ok_for_ldpstpPP7rtx_defb12machine_mode@@Base |
| LDx_mult_1 | ld4 | 19 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 15 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ZSTD_compressBlock_fast_extDict@@Base, _obstack_begin@@Base-0x52ce8, bfd_elf32_core_file_p@@Base |
| STx_mult_2 | st4 | 3 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | bfd_elf32_core_file_p@@Base, objalloc_free_block@@Base, pex_init@@Base |
| LDx_mult_2 | ld2 | 2 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z37aarch64_operands_adjust_ok_for_ldpstpPP7rtx_defb12machine_mode@@Base, bfd_merge_sections@@Base |
| STx_mult_2 | st2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | bfd_elf32_core_file_p@@Base, isl_dim_map_extend@@Base |
| LDx_mult_2 | ld4 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | isl_local_space_get_active@@Base |
| STx_mult_1 | st4 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z28vect_grouped_store_supportedP9tree_nodem@@Base |
| STx_mult_2 | st3 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | decNumberCompare@@Base |

### Scalar two-register misc: 52

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| NEG_1 | neg | 52 | 0 | 50 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | _Z14create_mem_refP20gimple_stmt_iteratorP9tree_nodeP8aff_treeS2_S2_S2_b@@Base, _Z17iv_can_overflow_pP4loopP9tree_nodeS2_S2_@@Base, _Z22emit_move_resolve_push12machine_modeP7rtx_def@@Base |

### SIMD Scalar pairwise: 13

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| ADDP_pair | addp | 13 | 2 | 2 | 0 | 0 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | ZSTD_splitBlock@@Base, _cpp_clean_line@@Base, _obstack_begin@@Base-0xea2e4 |

### SIMD Table Lookup: 9

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| TBL | tbl | 9 | 0 | 0 | 0 | 0 | 0 | 2 | 1 | 0 | 1 | 0 | 0 | 0 | 4 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | ZDICT_optimizeTrainFromBuffer_fastCover@@Base, _dl_mcount@@GLIBC_2.17, crc32_combine@@ZLIB_1.2.2 |

### Scalar three: 3

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FABD_2 | fabd | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | __fmod_finite@GLIBC_2.17, __yn_finite@GLIBC_2.17 |

### SIMD vector x indexed element: 2

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| MUL_elt | mul | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | isl_basic_map_overlying_set@@Base, isl_mat_alloc@@Base |

### SIMD Scalar three same: 1

| Entry | Mnemonic | Total | gcc | cc1 | as | ld | make | libc | ld.so | libm | libisl | libmpc | libmpfr | libgmp | libz | libzstd | libopcodes | libbfd | libctf | libjansson | libguile-3.0 | libsframe | libgc | libffi | libunistring | libcrypt | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| UQSUB_1 | uqsub | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | _Z21c_build_shufflevectormP9tree_nodeS0_RK3vecIS0_7va_heap6vl_ptrEb@@Base |

## Debian glibc 2.41 (minimal-debian rootfs)


### SIMD Two-register misc: 3

| Entry | Mnemonic | Total | deb_libc | deb_ld.so | Example functions |
|---|---|---:|---:|---:|---|
| FNEG_2 | fneg | 3 | 3 | 0 | copysign@@GLIBC_2.17, modf@@GLIBC_2.17 |

### Advanced SIMD Load/Store single structures: 3

| Entry | Mnemonic | Total | deb_libc | deb_ld.so | Example functions |
|---|---|---:|---:|---:|---|
| LD1R_1 | ld1r | 2 | 2 | 0 | _IO_fopen@@GLIBC_2.17, __nss_hash@@GLIBC_PRIVATE |
| LD1_sngl_1 | ld1 | 1 | 0 | 1 | _dl_mcount@@GLIBC_2.17 |

### SIMD three same: 2

| Entry | Mnemonic | Total | deb_libc | deb_ld.so | Example functions |
|---|---|---:|---:|---:|---|
| USHL_2 | ushl | 2 | 2 | 0 | statvfs@@GLIBC_2.17 |
