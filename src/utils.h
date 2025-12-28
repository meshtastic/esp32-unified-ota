#pragma once

void corrupt_partition(const esp_partition_t *partition);
void print_hash(const char *prefix, const uint8_t *hash);