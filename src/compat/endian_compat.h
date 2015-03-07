#ifndef ENDIAN_COMPAT_H
#define ENDIAN_COMPAT_H

#include <stdint.h>

void __byte_swap_slow(void* x, int size)
{
	uint8_t* b = (uint8_t*)x;
	int i;
	
	for(i = 0; i < size / 2; i++)
	{
		int j = size - i - 1;
		uint8_t tmp = b[i];
		b[i] = b[j];
		b[j] = tmp;
	}	
}

void custom_hton(void* x, int size)
{
	static int _test_big_endian = 42;
	static int _is_big_endian = -1;

	switch(_is_big_endian)
	{
		case 0:
			__byte_swap_slow(x, size);
			break;
		case 1:
			break;
		default:
			if(*(char *)&_test_big_endian == 42) {
				_is_big_endian = 0;
				__byte_swap_slow(x,size);
			}else _is_big_endian = 1;
	}
}

#define custom_ntoh(x, size) custom_hton(x,size)

#endif
