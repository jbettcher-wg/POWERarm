/*
 * Layout of the AArch64 guest vDSO. POWERarm maps the file whole at one
 * address, so file offsets are virtual addresses: one PT_LOAD from offset 0
 * holding the headers, the dynamic symbol tables and the code. Nothing
 * applies relocations to it. Modelled on arch/arm64/kernel/vdso/vdso.lds.S.
 */
SECTIONS {
  . = SIZEOF_HEADERS;
  .hash : { *(.hash) } :text
  .gnu.hash : { *(.gnu.hash) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .gnu.version : { *(.gnu.version) }
  .gnu.version_d : { *(.gnu.version_d) }
  .gnu.version_r : { *(.gnu.version_r) }
  .dynamic : { *(.dynamic) } :text :dynamic
  .rodata : {
    *(.rodata*)
      *(.data*)
      *(.sdata*)
      *(.got.plt) *(.got)
      *(.gnu.linkonce.d.*)
      *(.bss*)
      *(.dynbss*)
      *(.gnu.linkonce.b.*)
  } :text
  .text : { *(.text*) } :text

  /DISCARD/ : {
    *(.note)
    *(.note.gnu.property)
    *(.eh_frame_hdr)
    *(.eh_frame)
    *(.symtab)
  }
}

PHDRS {
  text PT_LOAD FLAGS(4 | 1) FILEHDR PHDRS;
  dynamic PT_DYNAMIC FLAGS(4);
}

/* The version node glibc (dl_vdso_vsym) and musl look the symbols up under. */
VERSION {
  LINUX_2.6.39 {
  global:
    __kernel_rt_sigreturn;
    __kernel_gettimeofday;
    __kernel_clock_gettime;
    __kernel_clock_getres;
  local: *;
  };
}
