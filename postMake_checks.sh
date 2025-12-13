#!/bin/bash

PROJECT_NAME=./integr_v02

echo '================================================'
echo ' *** BASIC CHECKS AFTER BUILDING THE PROJECT ***'

echo
echo "Check that symbols exist and addresses are sane:"
echo "------------------------------------------------"
arm-none-eabi-nm --defined-only ${PROJECT_NAME}.axf |  \
  egrep '_heap_start|_heap_end|_heap_size|_end_bss|_stack_top' | sort

echo
echo "Confirm ordering:"
echo "-----------------"
arm-none-eabi-objdump -h ${PROJECT_NAME}.axf
echo '  (check that .bss < .heap < stack region.)'

echo
echo "Test on malloc calling _sbrk and other stuff:"
echo "---------------------------------------------"
echo "> Show undefined (UNDEF) symbols related to allocator/ locks/ syscalls :"
ELF=${PROJECT_NAME}.axf
echo
arm-none-eabi-nm -u "$ELF" | egrep --color=never 'malloc|realloc|free|_sbrk|sbrk|__malloc|_malloc|_realloc|__malloc_lock|__malloc_unlock|_malloc_r|_realloc_r|_sbrk_r|__wrap__sbrk' > './undefined_malloc_symbols.txt'

echo
echo "> Wrote such symbols to 'undefined_malloc_symbols.txt' ---> content follows:"
echo
cat ./undefined_malloc_symbols.txt

echo
echo "> Show defined symbols that matter (sbrk/malloc etc) :"
echo
arm-none-eabi-nm --defined-only "$ELF" | egrep --color=never 'sbrk|_heap|_stack|malloc|realloc|free|__malloc_lock|__malloc_unlock|_malloc_r|_realloc_r' > ./defined_malloc_symbols.txt

echo
echo "> Wrote such symbols to 'defined_malloc_symbols.txt' ---> content follows:"
echo
cat ./defined_malloc_symbols.txt

