// Embeds data/ui_font.vlw under the same symbols PlatformIO's embed_files uses.
#if defined(__APPLE__)
  .section __DATA,__const
#else
  .section .rodata
#endif
  .p2align 2
  .globl _binary_data_ui_font_vlw_start
_binary_data_ui_font_vlw_start:
  .incbin FONT_PATH
  .globl _binary_data_ui_font_vlw_end
_binary_data_ui_font_vlw_end:
