#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>

#include "j1939decode.h"
#include "cJSON.h"

/* Log function pointer */
static log_fn_ptr log_fn = NULL;

/* J1939 lookup table pointer */
static cJSON * j1939db_json = NULL;

/* JSON object for all PGNs */
static cJSON * j1939db_pgns = NULL;
/* JSON object for all SPNs */
static cJSON * j1939db_spns = NULL;
/* JSON object for all source addresses */
static cJSON * j1939db_source_addresses = NULL;

/* Static helper functions */
static void log_msg(const char * fmt, ...);
static char * file_read(const char * filename, const char * mode);
static bool in_array(uint32_t val, const uint32_t * array, size_t len);
static cJSON * create_byte_array(const uint64_t * data);
static const cJSON * get_pgn_data(uint32_t pgn);
static const cJSON * get_spn_data(uint32_t spn);
static cJSON * extract_spn_data(uint32_t spn, const uint64_t * data, uint32_t start_bit);
static char * get_sa_name(uint8_t sa);
static char * get_pgn_name(uint32_t pgn);

/* Extract J1939 sub fields from CAN ID */
static inline uint8_t get_pri(uint32_t id)
{
    /* 3-bit priority */
    return (uint8_t) ((id >> (18U + 8U)) & ((1U << 3U) - 1));
}
static inline uint32_t get_pgn(uint32_t id)
{
    /* 18-bit parameter group number */
    return (uint32_t) ((id >> 8U) & ((1U << 18U) - 1));
}
static inline uint8_t get_sa(uint32_t id)
{
    /* 8-bit source address */
    return (uint8_t) ((id >> 0U) & ((1U << 8U) - 1));
}

/**************************************************************************//**

  \brief Log formatted message to user defined handler, or stderr as default

  \return void

******************************************************************************/
void log_msg(const char * fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (log_fn)
    {
        (*log_fn)(buf);
    }
    else
    {
        /* Default print to stderr */
        fprintf(stderr, "%s\n", buf);
    }
}

/**************************************************************************//**

  \brief  Read file contents into allocated memory

  \return pointer to string containing file contents

******************************************************************************/
char * file_read(const char * filename, const char * mode)
{
    FILE * fp = fopen(filename, mode);
    if (fp == NULL)
    {
        log_msg("Could not open file %s", filename);
        /* No need to close the file before returning since it was never opened */
        return NULL;
    }

    /* Get total file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    /* Allocate enough memory for entire file */
    char * buf = malloc(file_size + 1);
    if (buf == NULL)
    {
        log_msg("Memory allocation failure");
        fclose(fp);
        return NULL;
    }

    /* Read the entire file into a buffer */
    long read_size = fread(buf, 1, file_size, fp);
    if (read_size != file_size)
    {
        log_msg("Read %ld of %ld total bytes in file %s", read_size, file_size, filename);
        fclose(fp);
        return NULL;
    }

    fclose(fp);

    // Null terminator
    buf[file_size] = '\0';

    return buf;
}

/**************************************************************************//**

  \brief Set log function handler

  \return void

******************************************************************************/
void j1939decode_set_log_fn(log_fn_ptr fn)
{
    if (fn)
    {
        log_fn = fn;
    }
}

/**************************************************************************//**

  \brief Print version string

  \return version string

******************************************************************************/
const char * j1939decode_version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", J1939DECODE_VERSION_MAJOR, J1939DECODE_VERSION_MINOR, J1939DECODE_VERSION_PATCH);

    return version;
}

/**************************************************************************//**

  \brief Initialize and allocate memory for J1939 lookup table

  \return void

******************************************************************************/
void j1939decode_init(void)
{
    char * s;
    /* Memory-map all file contents */
    s = file_read(J1939DECODE_DB, "r");
    if (s != NULL)
    {
        j1939db_json = cJSON_Parse(s);
        free(s);
        if (j1939db_json == NULL)
        {
            log_msg("Unable to parse J1939db");
        }
    }

    /* Pre-allocate large JSON objects */
    j1939db_pgns = cJSON_GetObjectItemCaseSensitive(j1939db_json, "J1939PGNdb");
    j1939db_spns = cJSON_GetObjectItemCaseSensitive(j1939db_json, "J1939SPNdb");
    j1939db_source_addresses = cJSON_GetObjectItemCaseSensitive(j1939db_json, "J1939SATabledb");
}

/**************************************************************************//**

  \brief Deinitialize and free memory for J1939 lookup table

  \return void

******************************************************************************/
void j1939decode_deinit(void)
{
    /* cJSON_Delete() checks if pointer is NULL before freeing */
    cJSON_Delete(j1939db_json);

    /* Explicitly set pointer to NULL */
    j1939db_json = NULL;
}

/**************************************************************************//**

  \brief

  \param val    value to search
  \param array  array of values to search in
  \param len    number of elements in array

  \return bool  boolean indicating if value was found in array

******************************************************************************/
bool in_array(uint32_t val, const uint32_t * array, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (val == array[i])
        {
            return true;
        }
    }
    return false;
}

/**************************************************************************//**

  \brief Split 64 bit number into JSON array object of bytes

  \param data     pointer to data (8 bytes total)

  \return cJSON * pointer to the JSON array object

******************************************************************************/
cJSON * create_byte_array(const uint64_t * data)
{
    cJSON * array = cJSON_CreateArray();
    if (array == NULL)
    {
        goto cleanup;
    }

    for (uint32_t i = 0; i < sizeof(data); i++)
    {
        /* Cast to uint8 pointer and then index */
        cJSON * num = cJSON_CreateNumber(((uint8_t *) data)[i]);
        if (num == NULL)
        {
            goto cleanup;
        }
        cJSON_AddItemToArray(array, num);
    }

    return array;

    cleanup:
    cJSON_Delete(array);
    return NULL;
}

/**************************************************************************//**

  \brief Get parameter group number data

  \param pgn      parameter group number

  \return cJSON * pointer to the PGN data JSON object

******************************************************************************/
const cJSON * get_pgn_data(uint32_t pgn)
{
    /* String large enough to fit a six-digit number (18 bits) */
    char pgn_string[7];
    snprintf(pgn_string, sizeof(pgn_string), "%d", pgn);

    const cJSON * pgn_data = cJSON_GetObjectItemCaseSensitive(j1939db_pgns, pgn_string);

    return pgn_data;
}

/**************************************************************************//**

  \brief Get suspect parameter number data

  \param spn      suspect parameter number

  \return cJSON * pointer to the SPN data JSON object

******************************************************************************/
const cJSON * get_spn_data(uint32_t spn)
{
    /* String large enough to fit a six-digit number */
    char spn_string[7];
    snprintf(spn_string, sizeof(spn_string), "%d", spn);

    const cJSON * spn_data = cJSON_GetObjectItemCaseSensitive(j1939db_spns, spn_string);

    return spn_data;
}

/**************************************************************************//**

  \brief Extract suspect parameter number data items and decode SPN value

  \param spn        suspect parameter number
  \param data       pointer to data (8 bytes total)
  \param start_bit  starting bit of SPN in PGN (zero-order)

  \return cJSON *   pointer to the SPN data JSON object

******************************************************************************/
cJSON * extract_spn_data(uint32_t spn, const uint64_t * data, uint32_t start_bit)
{
    /* JSON object for specific SPN data */
    const cJSON * spn_data = get_spn_data(spn);

    /* Mutable copy of SPN data object
     * We are keeping all existing fields and then adding more of our own */
    /* TODO: Create our own SPN data JSON object rather than copying the existing one
     * By creating our own JSON object and adding all fields explicitly we have control over what the key names are */
    cJSON * spn_data_copy = cJSON_Duplicate(spn_data, 1);

    /* TODO: Use PascalCase or snake_case for JSON key names?
     * Existing J1939 lookup table uses PascalCase but snake_case may be more appropriate */

    if (cJSON_IsObject(spn_data))
    {
        int length_in_bits = cJSON_GetObjectItemCaseSensitive(spn_data, "SPNLength")->valueint;
        int offset = cJSON_GetObjectItemCaseSensitive(spn_data, "Offset")->valueint;
        double resolution = cJSON_GetObjectItemCaseSensitive(spn_data, "Resolution")->valuedouble;
        double operational_high = cJSON_GetObjectItemCaseSensitive(spn_data, "OperationalHigh")->valuedouble;
        double operational_low = cJSON_GetObjectItemCaseSensitive(spn_data, "OperationalLow")->valuedouble;
        /* Not currently using name or units
        char * name = cJSON_GetObjectItemCaseSensitive(spn_data, "Name")->valuestring;
        char * units = cJSON_GetObjectItemCaseSensitive(spn_data, "Units")->valuestring;
        */

        /* TODO: Support bit decodings for when the units are "Bits" */
        /* TODO: Support decoding of ASCII values when resolution is "ASCII" */

        /* Decode the data for this SPN */
        uint64_t mask = (1U << (unsigned int) length_in_bits) - 1;
        uint64_t value_raw = ((*data) >> start_bit) & mask;
        double value = value_raw * resolution + offset;

        if (cJSON_AddNumberToObject(spn_data_copy, "StartBit", start_bit) == NULL)
        {
            goto cleanup;
        }

        if (cJSON_AddNumberToObject(spn_data_copy, "ValueRaw", value_raw) == NULL)
        {
            goto cleanup;
        }

        /* Decoded value is valid boolean defaulting to false */
        cJSON_bool valid = false;

        /* Check that decoded value is within operational range */
        if (value >= operational_low && value <= operational_high)
        {
            if (cJSON_AddNumberToObject(spn_data_copy, "ValueDecoded", value) == NULL)
            {
                goto cleanup;
            }

            valid = true;
        }
        else
        {
            /* Decoded value is invalid or not available if outside of operation range
             * Use the "Valid" boolean key when checking if decoded data is valid or not */
            if (cJSON_AddStringToObject(spn_data_copy, "ValueDecoded", "Not available") == NULL)
            {
                goto cleanup;
            }
        }

        if (cJSON_AddBoolToObject(spn_data_copy, "Valid", valid) == NULL)
        {
            goto cleanup;
        }
    }
    else
    {
        log_msg("No SPN data found in database for SPN %d", spn);
        goto cleanup;
    }

    return spn_data_copy;

    cleanup:
    cJSON_Delete(spn_data_copy);
    /* Do not return the freed cJSON pointer to avoid use-after-free flaw
     * cJSON_Delete() does not set the freed pointer to NULL */
    return NULL;
}

/**************************************************************************//**

  \brief Get source address name

  \param sa      source address number

  \return char * pointer to the source address name string

******************************************************************************/
char * get_sa_name(uint8_t sa)
{
    char * sa_name;

    /* Preferred Addresses are in the range of 0 to 127 and 248 to 255 */
    if (sa <= 127 || sa >= 248)
    {
        /* Source addresses 92 through to 127 have not yet been assigned */
        if (sa >= 92 && sa <= 127)
        {
            sa_name = "Reserved";
        }
        else
        {
            /* String large enough to fit a three-digit number (8 bits) */
            char sa_string[4];
            snprintf(sa_string, sizeof(sa_string), "%d", sa);

            sa_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j1939db_source_addresses, sa_string));
            if (sa_name == NULL)
            {
                sa_name = "Unknown";
                log_msg("No source address name found in database for source address %d", sa);
            }
        }
    }
        /* Industry Group specific addresses are in the range of 128 to 247 */
    else if (sa >= 128 && sa <= 247)
    {
        sa_name = "Industry Group specific";
    }
    else
    {
        sa_name = "Unknown";
        log_msg("Unknown source address %d outside of expected range", sa);
    }

    return sa_name;
}

/**************************************************************************//**

  \brief Get parameter group number name

  \param pgn     parameter group number

  \return char * pointer to the PGN name string

******************************************************************************/
char * get_pgn_name(uint32_t pgn)
{
    char * pgn_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(get_pgn_data(pgn), "Name"));
    if (pgn_name == NULL)
    {
        pgn_name = "Unknown";
        log_msg("No PGN name found in database for PGN %d", pgn);
    }

    return pgn_name;
}

/**************************************************************************//**

  \brief Build JSON string for j1939 decoded data

  \param id         CAN identifier
  \param dlc        data length code
  \param data       pointer to data (8 bytes total)
  \param pretty     pretty print returned JSON string

  \return char *    pointer to the JSON string

******************************************************************************/
char * j1939decode_to_json(uint32_t id, uint8_t dlc, const uint64_t * data, bool pretty)
{
    /* Fail and return NULL if database is not loaded
     * Remember to call j1939decode_init() first! */
    if (j1939db_json == NULL)
    {
        log_msg("J1939 database not loaded");
        return NULL;
    }

    if (dlc > 8)
    {
        log_msg("DLC cannot be greater than 8 bytes");
        return NULL;
    }

    /* JSON string to be returned */
    char * json_string = NULL;

    /* JSON object containing decoded J1939 data */
    cJSON * json_object = cJSON_CreateObject();
    if (json_object == NULL)
    {
        goto end;
    }

    if (cJSON_AddNumberToObject(json_object, "ID", id) == NULL)
    {
        goto end;
    }

    if (cJSON_AddNumberToObject(json_object, "Priority", get_pri(id)) == NULL)
    {
        goto end;
    }

    if (cJSON_AddNumberToObject(json_object, "PGN", get_pgn(id)) == NULL)
    {
        goto end;
    }

    if (cJSON_AddNumberToObject(json_object, "SA", get_sa(id)) == NULL)
    {
        goto end;
    }

    if (cJSON_AddStringToObject(json_object, "SAName", get_sa_name(get_sa(id))) == NULL)
    {
        goto end;
    }

    if (cJSON_AddNumberToObject(json_object, "DLC", dlc) == NULL)
    {
        goto end;
    }

    /* Add raw data bytes to JSON object */
    cJSON_AddItemToObject(json_object, "DataRaw", create_byte_array(data));

    /* JSON object for specific PGN data */
    const cJSON * pgn_data = get_pgn_data(get_pgn(id));

    /* Decoded flag default to false until set otherwise */
    cJSON_bool decoded_flag = false;

    if (cJSON_IsObject(pgn_data))
    {
        /* PGN number found in lookup table */

        if (cJSON_AddStringToObject(json_object, "PGNName", get_pgn_name(get_pgn(id))) == NULL)
        {
            goto end;
        }

        /* JSON object containing list of decoded SPN data */
        cJSON * spn_object = cJSON_CreateObject();
        if (spn_object == NULL)
        {
            goto end;
        }

        /* JSON object for SPN list in PGN */
        const cJSON * spn_list_array = cJSON_GetObjectItemCaseSensitive(pgn_data, "SPNs");
        if (cJSON_IsArray(spn_list_array))
        {
            uint32_t num_spns = cJSON_GetArraySize(spn_list_array);
            if (num_spns > 0)
            {
                /* One or more SPNs exist for PGN */

                for (uint32_t i = 0; i < num_spns; i++)
                {
                    /* Getting SPN number from SPN list in PGN since the SPN number is used as a key only and not included in the SPN data object itself */
                    uint32_t spn_number = (uint32_t) cJSON_GetArrayItem(spn_list_array, i)->valueint;

                    /* Array of all possible proprietary SPNs */
                    const uint32_t proprietary_spns[] = {2550, 2551, 3328};

                    /* Check for proprietary SPNs */
                    if (in_array(spn_number, proprietary_spns, sizeof(proprietary_spns) / sizeof(proprietary_spns[0])))
                    {
                        /* TODO: Disabling print to silently ignore proprietary SPNs */
                        /* log_msg("Skipping decode for proprietary SPN %d", spn_number); */
                    }
                    else
                    {
                        /* Need to pass SPN starting bit position since it is found in the PGN data object */
                        uint32_t start_bit;
                        const cJSON * start_bit_json = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(pgn_data, "SPNStartBits"), i);
                        if (cJSON_IsNumber(start_bit_json))
                        {
                            if (start_bit_json->valueint < 0)
                            {
                                log_msg("Start bit cannot be negative for SPN %d, skipping decode", spn_number);
                                continue;
                            }

                            /* Now cast to unsigned */
                            start_bit = (uint32_t) start_bit_json->valueint;
                        }
                        else
                        {
                            log_msg("No start bit found in database for SPN %d, skipping decode", spn_number);
                            continue;
                        }

                        /* Add SPN data object to SPN list object using SPN number as a key */
                        char spn_string[7];
                        snprintf(spn_string, sizeof(spn_string), "%d", spn_number);

                        cJSON * spn_data = extract_spn_data(spn_number, data, start_bit);
                        if (spn_data != NULL)
                        {
                            /* At least one SPN found in database and actually decoded */

                            /* TODO: What criteria should be used to determine if the message should be flagged as "decoded" or not?
                             * 1. If PGN data found in database?
                             * 2. If at least one SPN found in database for PGN?
                             * 3. If at least one SPN, with start bits, found in database for PGN?
                             * 4. If at least one SPN actually decoded? (i.e. extract_spn_data() did not return NULL)
                             * Using #4 criteria for now */

                            decoded_flag = true;
                        }
                        cJSON_AddItemToObject(spn_object, spn_string, spn_data);
                    }
                }
            }
            else
            {
                log_msg("Empty SPN list found in database for PGN %d", get_pgn(id));
            }
        }
        else
        {
            log_msg("No SPNs found in database for PGN %d", get_pgn(id));
        }

        /* Add SPN list object to the main JSON object */
        cJSON_AddItemToObject(json_object, "SPNs", spn_object);
    }
    else
    {
        /* TODO: This print may happen too often when trying to decode non-J1939 data */
        /* log_msg("PGN %d not found in database", get_pgn(id)); */
    }

    if (cJSON_AddBoolToObject(json_object, "Decoded", decoded_flag) == NULL)
    {
        goto end;
    }

    /* Print the JSON string
     * Memory will be allocated so remember to free it when you are done with it! */
    json_string = pretty ? cJSON_Print(json_object) : cJSON_PrintUnformatted(json_object);
    if (json_string == NULL)
    {
        log_msg("Failed to print JSON string");
    }

    end:
    cJSON_Delete(json_object);
    return json_string;
}
