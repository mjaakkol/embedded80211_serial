#ifndef _VSERIAL_H_
#define _VSERIAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool vserial_acquire_buffer(uint8_t **data, size_t len);
void vserial_commit_data(size_t len);
void vserial_message_complete(size_t len);

#endif /* _VSERIAL_H_ */
