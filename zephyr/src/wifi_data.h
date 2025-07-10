#ifndef _WIFI_DATA_H_
#define _WIFI_DATA_H_

bool wifi_data_acquire_buffer(uint8_t **data, size_t len);
void wifi_data_commit_data(size_t len);
void wifi_data_message_complete(size_t len);


#endif // _WIFI_DATA_H_