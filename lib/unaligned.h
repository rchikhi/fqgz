/*
 * unaligned.h - inline functions for unaligned memory accesses
 */

#ifndef LIB_UNALIGNED_H
#define LIB_UNALIGNED_H

#include "lib_common.h"

/*
 * Naming note:
 *
 * {load,store}_*_unaligned() deal with raw bytes without endianness conversion.
 * {get,put}_unaligned_*() deal with a specific endianness.
 */

DEFINE_UNALIGNED_TYPE(u16)
DEFINE_UNALIGNED_TYPE(u32)
DEFINE_UNALIGNED_TYPE(u64)
DEFINE_UNALIGNED_TYPE(machine_word_t)

#define load_word_unaligned	load_machine_word_t_unaligned
#define store_word_unaligned	store_machine_word_t_unaligned

/***** Unaligned loads  *****/

#define get_unaligned_le16(p) load_u16_unaligned((p))
#define get_unaligned_le32(p) load_u32_unaligned((p))
#define get_unaligned_le64(p) load_u64_unaligned((p))
#define get_unaligned_leword(p) load_u64_unaligned((p)) //FIXME: support 32bit arch ?
#define get_unaligned_be16(p) be16_bswap(load_u16_unaligned((p)))
#define get_unaligned_be32(p) be32_bswap(load_u32_unaligned((p)))
#define get_unaligned_be64(p) be64_bswap(load_u64_unaligned((p)))
#define get_unaligned_beword(p) be64_bswap(load_u64_unaligned((p))) //FIXME: support 32bit arch ?



/***** Unaligned stores  *****/


#define put_unaligned_le16(v,p) store_u16_unaligned((v), (p))
#define put_unaligned_le32(v,p) store_u32_unaligned((v), (p))
#define put_unaligned_le64(v,p) store_u64_unaligned((v), (p))
#define put_unaligned_leword(v,p) store_u64_unaligned((v), (p)) //FIXME: support 32bit arch ?
#define put_unaligned_be16(v,p) store_u16_unaligned(be16_bswap(v), (p))
#define put_unaligned_be32(v,p) store_u32_unaligned(be32_bswap(v), (p))
#define put_unaligned_be64(v,p) store_u64_unaligned(be64_bswap(v), (p))
#define put_unaligned_beword(v,p) store_u64_unaligned(be32_bswap(v), (p)) //FIXME: support 32bit arch ?

/***** 24-bit loads *****/

/*
 * Given a 32-bit value that was loaded with the platform's native endianness,
 * return a 32-bit value whose high-order 8 bits are 0 and whose low-order 24
 * bits contain the first 3 bytes, arranged in octets in a platform-dependent
 * order, at the memory location from which the input 32-bit value was loaded.
 */

#define loaded_u32_to_u24(v) ((v) & 0xFFFFFF)


/*
 * Load the next 3 bytes from the memory location @p into the 24 low-order bits
 * of a 32-bit value.  The order in which the 3 bytes will be arranged as octets
 * in the 24 bits is platform-dependent.  At least LOAD_U24_REQUIRED_NBYTES
 * bytes must be available at @p; note that this may be more than 3.
 */
#define load_u24_unaligned(p) loaded_u32_to_u24(load_u32_unaligned((p)))

#endif /* LIB_UNALIGNED_H */
