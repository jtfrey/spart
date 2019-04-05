/******************************************************************
* spart    : a user-oriented partition info command for slurm
* Author   : Cem Ahmet Mercan
* Licence  : GNU General Public License v2.0
* FirstVer.: 2019
* Note     : Some part of this code taken from slurm api man pages
*******************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>

#include <unistd.h>
#include <getopt.h>

#include "version.h"

#define SPART_CLI_OPT_HELP          'h'
#define SPART_CLI_OPT_FORMAT        'f'
#define SPART_CLI_OPT_PARTITION     'p'
#define SPART_CLI_OPT_MINMAX        'm'
#define SPART_CLI_OPT_SIUNITS       256

#define SPART_CLI_OPTSTRING         "hf:p:m"

struct option spart_cli_opts[] = {
            { "help",           no_argument,        NULL,       SPART_CLI_OPT_HELP },
            { "format",         required_argument,  NULL,       SPART_CLI_OPT_FORMAT },
            { "partition",      required_argument,  NULL,       SPART_CLI_OPT_PARTITION },
            { "minmax",         no_argument,        NULL,       SPART_CLI_OPT_MINMAX },
            { "si-units",       no_argument,        NULL,       SPART_CLI_OPT_SIUNITS },
            { NULL,             0,                  NULL,       0   }
        };

/*
 * @brief Determine inclusion of node resources in aggregates
 *
 * Augment this function if you want to include additional parameters in state-based
 * inclusion of a node's resources.
 *
 * @param node_info Slurm node information record
 * @return non-zero for node inclusion, zero if node should be excluded
 */
int
spart_should_include_node(
    node_info_t     *node_info
)
{
    return ( ((node_info->node_state & NODE_STATE_DRAIN) != NODE_STATE_DRAIN)
          && ((node_info->node_state & NODE_STATE_BASE) != NODE_STATE_DOWN)
        );
}

/*
 * @brief Per-partition summary record
 *
 * For each partition being summarized, this data structure includes a pointer to
 * its Slurm partition record (for the name, limits, etc.) and a few aggregated
 * values computed by this program.
 *
 */
typedef struct spart_data {
    partition_info_t        *partition;
    unsigned long int       min_cpu, max_cpu, free_cpu;
    unsigned long int       min_mem, max_mem, free_mem, total_mem;
    unsigned long int       free_node;
    unsigned long int       pending_jobs_resources, pending_jobs_other;
    unsigned long int       pending_cpus_resources, pending_cpus_other;
} spart_data_t;

/*
 * @brief Initialize a partition summary record
 *
 * Initializes the part_data record.  Walks all nodes associated with the
 * partition to aggregate CPU, node, and memory limits and usage counts.
 *
 * @param part_data Summary record to init
 * @param part_info Slurm partition information record
 * @param node_array The array of Slurm node records
 * @return zero if the summary record could not be initialized
 */
int
spart_data_init(
    spart_data_t        *part_data,
    partition_info_t    *part_info,
    node_info_t         *node_array
)
{

    memset(part_data, 0, sizeof(spart_data_t));
    part_data->min_cpu = ULONG_MAX;
    part_data->max_cpu = 0;
    part_data->min_mem = ULONG_MAX;
    part_data->max_mem = 0;
    part_data->partition = part_info;
    if ( part_info ) {
        unsigned int    node_idx = 0;

        while ( part_info->node_inx[node_idx] != -1 ) {
            int32_t     i = part_info->node_inx[node_idx++];
            int32_t     iMax = part_info->node_inx[node_idx++];

            while ( i <= iMax ) {
                unsigned int    node_mem = node_array[i].real_memory;
                unsigned int    node_cpu = node_array[i].cpus;
                /* sview.c shows SELECT_NODEDATA_SUBCNT:NODE_STATE_ALLOCATED to be a 16-bit unsigned int: */
                uint16_t        node_cpu_alloc = 0;

                if ( part_data->min_mem > node_mem ) part_data->min_mem = node_mem;
                if ( part_data->max_mem < node_mem ) part_data->max_mem = node_mem;
                if ( part_data->min_cpu > node_cpu ) part_data->min_cpu = node_cpu;
                if ( part_data->max_cpu < node_cpu ) part_data->max_cpu = node_cpu;

                /* Error-check the nodeinfo fetch and exit if it fails */
                if ( slurm_get_select_nodeinfo(node_array[i].select_nodeinfo, SELECT_NODEDATA_SUBCNT, NODE_STATE_ALLOCATED, &node_cpu_alloc) != 0 ) return 0;

                /* Only include in the usage stats if we're supposed to: */
                if ( spart_should_include_node(&node_array[i]) ) {
                    /* No CPUs allocated to jobs?  That's a free node: */
                    if ( node_cpu_alloc == 0 ) part_data->free_node++;
                    part_data->free_cpu += (node_cpu - node_cpu_alloc);
                    part_data->free_mem += node_array[i].free_mem;
                    part_data->total_mem += node_mem;
                }
                i++;
            }
        }
    }
    return 1;
}

/*
 * @brief Partition enumeration flags
 *
 * Flags passed to partition summary display callbacks to indicate the disposition
 * of individual records.
 *
 */
typedef enum {
    spart_printer_enum_first = 1 << 0,
    spart_printer_enum_last = 1 << 1
} spart_printer_enum_t;

/*
 * @brief Partition summary, allocate and init context
 *
 * Type of a function that allocates and initializes a format-specific context
 * pointer.
 *
 * @result an opaque pointer or pointer-sized value
 */
typedef const void* (*spart_printer_context_alloc_callback)(const int argc, char * const argv[]);
/*
 * @brief Partition summary, pre-process the partition summary list
 *
 * Type of a function that walks the partition summary list before actually
 * printing anything.  This can be used to determine proper column widths,
 * for example, and stash such information in the context.
 *
 * @param part_data a partition summary record
 * @param context the format-specific context pointer
 * @result zero if enumeration should cease and the program exit
 */
typedef int (*spart_printer_preprint_callback)(spart_data_t *part_data, const void *context);
/*
 * @brief Partition summary, print summary header
 *
 * Type of a function that writes a partition summary header to stdout.
 *
 * @param context the format-specific context pointer
 */
typedef void (*spart_printer_header_callback)(const void *context);
/*
 * @brief Partition summary, print a partition summary record
 *
 * Type of a function that writes a partition summary record to stdout.
 *
 * @param part_data a partition summary record
 * @param enum_flags enumeration flags for this record
 * @param context the format-specific context pointer
 * @result zero if enumeration should cease and the program exit
 */
typedef int (*spart_printer_print_callback)(spart_data_t *part_data, spart_printer_enum_t enum_flags, const void *context);
/*
 * @brief Partition summary, print summary footer
 *
 * Type of a function that writes a partition summary footer to stdout.
 *
 * @param context the format-specific context pointer
 */
typedef void (*spart_printer_footer_callback)(const void *context);
/*
 * @brief Partition summary, deallocate format-specific context pointer
 *
 * Type of a function that deallocates a format-specific context pointer.
 *
 * @param context the format-specific context pointer
 */
typedef void (*spart_printer_context_dealloc_callback)(const void *context);

/*
 * @brief Partition summary formatter
 *
 * Each distrinct format which this program outputs is represented by one
 * of these data structures.  Any component function pointer that is NULL
 * implies a default action:
 *
 *   context_alloc_fn       no format-specific context will be allocated
 *   preprint_fn            no-op
 *   header_fn              no-op
 *   print_fn               no-op
 *   footer_fn              no-op
 *   context_dealloc_fn     non-NULL context pointer is free()'d
 */
typedef struct spart_printer {
    spart_printer_context_alloc_callback    context_alloc_fn;
    spart_printer_preprint_callback         preprint_fn;
    spart_printer_header_callback           header_fn;
    spart_printer_print_callback            print_fn;
    spart_printer_footer_callback           footer_fn;
    spart_printer_context_dealloc_callback  context_dealloc_fn;
} spart_printer_t;

/**/


/*
 * Column-aligned textual table format
 */

typedef struct spart_printer_text_context {
    int     max_part_name_len;
    int     should_show_node_minmax;
    int     unit_multiplier;
} spart_printer_text_context_t;

const void*
__spart_printer_text_context_alloc_callback(
    const int       argc,
    char * const    argv[]
)
{
    spart_printer_text_context_t   *context = malloc(sizeof(spart_printer_text_context_t));

    if ( context ) {
        context->max_part_name_len = 12;
        context->should_show_node_minmax = 0;
        context->unit_multiplier = 1024;
        if ( argc ) {
            int     argn = 1;

            while ( argn < argc ) {
                if ( (strcmp(argv[argn], "-m") == 0) || (strcmp(argv[argn], "--minmax") == 0) ) {
                    context->should_show_node_minmax = 1;
                }
                else if ( strcmp(argv[argn], "--si-units") == 0 ) {
                    context->unit_multiplier = 1000;
                }
                argn++;
            }
        }
    }
    return context;
}

int
__spart_printer_text_preprint(
    spart_data_t    *part_data,
    const void      *context
)
{
    spart_printer_text_context_t   *CONTEXT = (spart_printer_text_context_t*)context;
    size_t                          part_name_len = strlen(part_data->partition->name);

    if ( part_name_len > CONTEXT->max_part_name_len ) CONTEXT->max_part_name_len = part_name_len;
    return 1;
}

void
__spart_printer_text_header(
    const void      *context
)
{
    spart_printer_text_context_t   *CONTEXT = (spart_printer_text_context_t*)context;

    if ( CONTEXT->should_show_node_minmax ) {
        printf("%-*s     FREE    TOTAL     FREE    TOTAL    TOTAL RESOURCE    OTHER   MIN   MAX  MAXJOBTIME MIN CPU MAX CPU MIN MEM MAX MEM\n", CONTEXT->max_part_name_len, "");
        printf("%-*s     CPUS     CPUS    NODES    NODES FREE MEM  PENDING  PENDING NODES NODES   DAY-HR:MN PERNODE PERNODE PERNODE PERNODE\n", CONTEXT->max_part_name_len, "PARTITION");
    } else {
        printf("%-*s     FREE    TOTAL     FREE    TOTAL    TOTAL RESOURCE    OTHER   MIN   MAX  MAXJOBTIME\n", CONTEXT->max_part_name_len, "");
        printf("%-*s     CPUS     CPUS    NODES    NODES FREE MEM  PENDING  PENDING NODES NODES   DAY-HR:MN\n", CONTEXT->max_part_name_len, "PARTITION");
    }
}

int
__spart_printer_text_print(
    spart_data_t            *part_data,
    spart_printer_enum_t    enum_flags,
    const void              *context
)
{
    spart_printer_text_context_t   *CONTEXT = (spart_printer_text_context_t*)context;

    printf ("%-*s %8lu %8lu %8lu %8lu %8lu %8lu %8lu %5lu ",
            CONTEXT->max_part_name_len,
            part_data->partition->name,
            (unsigned long)part_data->free_cpu,
            (unsigned long)part_data->partition->total_cpus,
            (unsigned long)part_data->free_node,
            (unsigned long)part_data->partition->total_nodes,
            (unsigned long)part_data->free_mem / CONTEXT->unit_multiplier,
            (unsigned long)part_data->pending_cpus_resources,
            (unsigned long)part_data->pending_cpus_other,
            (unsigned long)part_data->partition->min_nodes
        );
    if ( part_data->partition->max_nodes == UINT_MAX ) {
        printf ("    - ");
    } else {
        printf ("%5lu ", (unsigned long)part_data->partition->max_nodes);
    }
    if ( part_data->partition->max_time == INFINITE ) {
        printf("   NO-LIMIT");
    } else {
        int         day, hour, minute;

        minute = part_data->partition->max_time;
        day = minute / 1440;
        minute -= (day * 1440);
        hour = minute / 60;
        minute -= (hour * 60);
        printf(" %4d-%02d:%02d",
                day, hour, minute
            );
    }
    if ( CONTEXT->should_show_node_minmax ) {
        printf(" %7lu %7lu %7lu %7lu",
                (unsigned long)part_data->min_cpu, (unsigned long)part_data->max_cpu,
                (unsigned long)part_data->min_mem / CONTEXT->unit_multiplier, (unsigned long)part_data->max_mem / CONTEXT->unit_multiplier
            );
    }
    fputc('\n', stdout);
    return 1;
}

spart_printer_t spart_printer_text = {
                    .context_alloc_fn = __spart_printer_text_context_alloc_callback,
                    .preprint_fn = __spart_printer_text_preprint,
                    .header_fn = __spart_printer_text_header,
                    .print_fn = __spart_printer_text_print,
                    .footer_fn = NULL,
                    .context_dealloc_fn = NULL
                };


/*
 * Parseable text format
 */

void
__spart_printer_parseable_header(
    const void      *context
)
{
    printf("PARTITION|FREE CPUS|TOTAL CPUS|FREE NODES|TOTAL NODES|RESOURCE PENDING, CPUS|OTHER PENDING, CPUS|RESOURCE PENDING, JOBS|OTHER PENDING, JOBS|MIN NODES|MAX NODES|MAXJOBTIME|MIN CPUS PERNODE|MAX CPUS PERNODE|MIN MEM PERNODE|MAX MEM PERNODE|FREE MEM|TOTAL MEM\n");
}

int
__spart_printer_parseable_print(
    spart_data_t            *part_data,
    spart_printer_enum_t    enum_flags,
    const void              *context
)
{
    printf ("%s|%lu|%lu|%lu|%lu|%lu|%lu|%lu|",
            part_data->partition->name,
            (unsigned long)part_data->free_cpu,
            (unsigned long)part_data->partition->total_cpus,
            (unsigned long)part_data->free_node,
            (unsigned long)part_data->partition->total_nodes,
            (unsigned long)part_data->pending_cpus_resources,
            (unsigned long)part_data->pending_cpus_other,
            (unsigned long)part_data->pending_jobs_resources,
            (unsigned long)part_data->pending_jobs_other,
            (unsigned long)part_data->partition->min_nodes
        );
    if ( part_data->partition->max_nodes == UINT_MAX ) {
        printf ("|");
    } else {
        printf ("%lu|", (unsigned long)part_data->partition->max_nodes);
    }
    if ( part_data->partition->max_time == INFINITE ) {
        printf ("|");
    } else {
        printf("%lu|", (unsigned long)part_data->partition->max_time);
    }
    printf(
            "%lu|%lu|%lu|%lu|%lu|%lu\n",
            (unsigned long)part_data->min_cpu, (unsigned long)part_data->max_cpu, (unsigned long)part_data->min_mem, (unsigned long)part_data->max_mem, (unsigned long)part_data->free_mem, (unsigned long)part_data->total_mem
        );
    return 1;
}

spart_printer_t spart_printer_parseable = {
                    .context_alloc_fn = NULL,
                    .preprint_fn = NULL,
                    .header_fn = __spart_printer_parseable_header,
                    .print_fn = __spart_printer_parseable_print,
                    .footer_fn = NULL,
                    .context_dealloc_fn = NULL
                };

/*
 * JSON dictionary format, keyed by partition name
 */

void
__spart_printer_json_header(
    const void      *context
)
{
    printf("{");
}

int
__spart_printer_json_print(
    spart_data_t            *part_data,
    spart_printer_enum_t    enum_flags,
    const void              *context
)
{
    printf("\"%s\":{", part_data->partition->name);
    printf( "\"free_cpu\":%lu,"
            "\"total_cpus\":%lu,"
            "\"free_node\":%lu,"
            "\"total_nodes\":%lu,"
            "\"pending_resources\":{\"cpus\":%lu,\"jobs\":%lu},"
            "\"pending_other\":{\"cpus\":%lu,\"jobs\":%lu},"
            "\"min_nodes\":%lu,"
            "\"cpu_per_node\":[%lu,%lu],"
            "\"mem_per_node\":[%lu,%lu],"
            "\"free_mem\":%lu,"
            "\"total_mem\":%lu,"
            ,
            (unsigned long)part_data->free_cpu,
            (unsigned long)part_data->partition->total_cpus,
            (unsigned long)part_data->free_node,
            (unsigned long)part_data->partition->total_nodes,
            (unsigned long)part_data->pending_cpus_resources, (unsigned long)part_data->pending_jobs_resources,
            (unsigned long)part_data->pending_cpus_other, (unsigned long)part_data->pending_jobs_other,
            (unsigned long)part_data->partition->min_nodes,
            (unsigned long)part_data->min_cpu, (unsigned long)part_data->max_cpu,
            (unsigned long)part_data->min_mem, (unsigned long)part_data->max_mem,
            (unsigned long)part_data->free_mem,
            (unsigned long)part_data->total_mem
        );

    if ( part_data->partition->max_nodes == UINT_MAX ) {
        printf("\"max_nodes\":null,");
    } else {
        printf("\"max_nodes\":%lu,", (unsigned long)part_data->partition->max_nodes);
    }
    if ( part_data->partition->max_time == INFINITE ) {
        printf("\"max_time\":null");
    } else {
        printf("\"max_time\":%lu", (unsigned long)part_data->partition->max_time);
    }
    printf("%s", (enum_flags & spart_printer_enum_last) ? "}" : "},");
}

void
__spart_printer_json_footer(
    const void      *context
)
{
    printf("}\n");
}

spart_printer_t spart_printer_json = {
                    .context_alloc_fn = NULL,
                    .preprint_fn = NULL,
                    .header_fn = __spart_printer_json_header,
                    .print_fn = __spart_printer_json_print,
                    .footer_fn = __spart_printer_json_footer,
                    .context_dealloc_fn = NULL
                };

/*
 * YAML dictionary format, keyed by partition name
 */
int
__spart_printer_yaml_print(
    spart_data_t            *part_data,
    spart_printer_enum_t    enum_flags,
    const void              *context
)
{
    printf("%s:\n", part_data->partition->name);
    printf( "    free_cpu: %lu\n"
            "    total_cpus: %lu\n"
            "    free_node: %lu\n"
            "    total_nodes: %lu\n"
            "    pending_resources:\n"
            "        cpus: %lu\n"
            "        jobs: %lu\n"
            "    pending_other:\n"
            "        cpus: %lu\n"
            "        jobs: %lu\n"
            "    min_nodes: %lu\n"
            "    cpu_per_node: [%lu, %lu]\n"
            "    mem_per_node: [%lu, %lu]\n"
            "    free_mem: %lu\n"
            "    total_mem: %lu\n"
            ,
            (unsigned long)part_data->free_cpu,
            (unsigned long)part_data->partition->total_cpus,
            (unsigned long)part_data->free_node,
            (unsigned long)part_data->partition->total_nodes,
            (unsigned long)part_data->pending_cpus_resources, (unsigned long)part_data->pending_jobs_resources,
            (unsigned long)part_data->pending_cpus_other, (unsigned long)part_data->pending_jobs_other,
            (unsigned long)part_data->partition->min_nodes,
            (unsigned long)part_data->min_cpu, (unsigned long)part_data->max_cpu,
            (unsigned long)part_data->min_mem, (unsigned long)part_data->max_mem,
            (unsigned long)part_data->free_mem,
            (unsigned long)part_data->total_mem
        );

    if ( part_data->partition->max_nodes == UINT_MAX ) {
        printf("    max_nodes: null\n");
    } else {
        printf("    max_nodes: %lu\n", (unsigned long)part_data->partition->max_nodes);
    }
    if ( part_data->partition->max_time == INFINITE ) {
        printf("    max_time: null\n");
    } else {
        printf("    max_time: %lu\n", (unsigned long)part_data->partition->max_time);
    }
}

spart_printer_t spart_printer_yaml = {
                    .context_alloc_fn = NULL,
                    .preprint_fn = NULL,
                    .header_fn = NULL,
                    .print_fn = __spart_printer_yaml_print,
                    .footer_fn = NULL,
                    .context_dealloc_fn = NULL
                };

/*
 * @brief Show program usage summary
 *
 * Write help text to stdout.
 *
 * @param exe the program being executed (e.g. argv[0] in main())
 */
void
usage(
    const char      *exe
)
{
    printf( "usage, version %s:\n\n"
            "    %s {options}\n\n"
            "  options:\n\n"
            "    -h/--help                  display this help\n"
            "    -f/--format <format>       display the parition summary in the\n"
            "                               specified format\n"
            "    -p/--partition <part-list> select specific partitions to include\n"
            "                               in the summary\n"
            "    -m/--minmax                for the standard text format, include\n"
            "                               per-node min/max values for CPU count and\n"
            "                               memory size\n"
            "    --si-units                 for the standard text format, display\n"
            "                               memory sizes in base-10 units (MB not MiB)\n"
            "\n"
            "    <format>:\n\n"
            "      text                     column-aligned textual table\n"
            "      parseable, parsable      text delimited by vertical bar characters;\n"
            "                               memory values in MB, times in minutes\n"
            "      json                     JSON dictionary keyed by partition name;\n"
            "                               memory values in MB, times in minutes\n"
            "      yaml                     YAML dictionary keyed by partition name;\n"
            "                               memory values in MB, times in minutes\n"
            "\n"
            "    <part-list>:\n\n"
            "      one or more partition names separated only by commas (no whitespace)\n"
            "\n"
            "      e.g.  \"part1,part2,part3\"\n"
            "\n"
            ,
            spart_version_str,
            exe
        );
}

/*
 * @brief Return integral ordering of two strings of specific lengths
 *
 * The strncasecmp() functions ONLY compare the first n characters of
 * its arguments.  Equality (0) only means that those n characters are
 * equivalent, NOT that the full strings are equivalent.
 *
 * This function checks the first n characters, where n is the minimum of
 * the two string lengths.  If the result is equality (0) then it also
 * checks any additional characters to ensure full equality of the two
 * strings.
 *
 * @param s1        string 1
 * @param s1_len    byte length of string 1
 * @param s2        string 2
 * @param s2_len    byte length of string 2
 * @result Zero if s1 and s2 are equivalent, negative if they are
 *         in descending order (s2 > s1), positive if they are in
 *         ascending order (s1 > s2)
 */
int
spart_strorder(
    const char      *s1,
    size_t          s1_len,
    const char      *s2,
    size_t          s2_len
)
{
    size_t          n = (s2_len < s1_len) ? s2_len : s1_len;
    int             cmp = strncasecmp(s1, s2, n);

    if ( cmp == 0 ) {
        if ( n < s1_len ) return s1[n];
        if ( n < s2_len ) return -s2[n];
    }
    return cmp;
}

/*
 * @brief Ordered linked list of strings
 *
 * A copy of the string is stored internal to the list node
 * with its byte length cached.
 */
typedef struct spart_string_list {
    struct spart_string_list    *link;
    char                        *string;
    size_t                      string_len;
} spart_string_list_t;

/*
 * @brief Add a string to a list
 *
 * The list_head is a pointer to the leading node of the target
 * list.  The *list_head will be updated if a new node is allocated
 * and becomes the new leading node of the list.
 *
 * If string_len is 0, the strlen(string) is used.
 *
 * If the incoming string is already present in the list, that node
 * is returned.
 *
 * @param list_head pointer to the leading node of the target list
 * @param string character array to be added to the list
 * @param string_len number of bytes in string that should be considered;
 *          pass 0 to use strlen(string)
 * @result NULL on error, the node containing string otherwise
 */
spart_string_list_t*
spart_string_list_add(
    spart_string_list_t     **list_head,
    const char              *string,
    size_t                  string_len
)
{
    spart_string_list_t     *new_node;
    spart_string_list_t     *node = *list_head, *prev_node = NULL;

    if ( string_len == 0 ) string_len = strlen(string);

    while ( node ) {
        int                 cmp = spart_strorder(node->string, node->string_len, string, string_len);

        if ( cmp == 0 ) return node;
        if ( cmp > 0 ) break;
        prev_node = node;
        node = node->link;
    }

    new_node = malloc(sizeof(spart_string_list_t) + string_len + 1);
    if ( new_node ) {
        new_node->string = ((void*)new_node) + sizeof(spart_string_list_t);
        memcpy(new_node->string, string, string_len);
        new_node->string[string_len] = '\0';
        new_node->string_len = strlen(new_node->string);

        if ( prev_node == NULL ) {
            new_node->link = *list_head;
            *list_head = new_node;
        } else {
            new_node->link = node;
            prev_node->link = new_node;
        }
    }
    return new_node;
}

/*
 * @brief Find a string in a list
 *
 * @param list_head the leading node of the target list
 * @param string character array to be added to the list
 * @param string_len number of bytes in string that should be considered;
 *          pass 0 to use strlen(string)
 * @result NULL if the string is not present in the list, a pointer to the
 *          matching character string stored in the list otherwise
 */
const char*
spart_string_list_find(
    spart_string_list_t     *list_head,
    const char              *string,
    size_t                  string_len
)
{
    if ( string_len == 0 ) string_len = strlen(string);

    while ( list_head ) {
        int                 cmp = spart_strorder(list_head->string, list_head->string_len, string, string_len);

        if ( cmp == 0 ) return list_head->string;
        if ( cmp > 0 ) break;
        list_head = list_head->link;
    }
    return NULL;
}

/*
 * @brief Destroy a string list
 *
 * The list_head is a pointer to the leading node of the target
 * list.  The *list_head will set to NULL by this function.
 *
 * @param list_head pointer to the leading node of the target list
 */
void
spart_string_list_destroy(
    spart_string_list_t     **list_head
)
{
    spart_string_list_t     *node = *list_head, *next_node = NULL;

    while ( node ) {
        next_node = node->link;
        free((void*)node);
        node = next_node;
    }
    *list_head = NULL;
}

/*
 * @brief Print a string in a list
 *
 * Print the string list to the given file stream.
 *
 * @param stream the FILE pointer to which the list is printed
 * @param list_head the leading node of the target list
 * @param delimiter the text to print after each string in the list; if
 *              NULL then a newline is used
 */
void
spart_string_list_fprint(
    FILE                *stream,
    spart_string_list_t *list_head,
    const char          *delimiter
)
{
    if ( ! delimiter ) delimiter = "\n";
    while ( list_head ) {
        fprintf(stream, "%s%s", list_head->string, delimiter);
        list_head = list_head->link;
    }
}

/**/

int
main(
    int             argc,
    char * const    argv[]
)
{
    spart_printer_t         *printer = &spart_printer_text;
    spart_string_list_t     *selected_partitions = NULL;
    spart_data_t            *part_data = NULL;
    partition_info_msg_t    *part_buffer_ptr = NULL;
    node_info_msg_t         *node_buffer_ptr = NULL;
    job_info_msg_t          *job_buffer_ptr = NULL;
    unsigned int            part_idx, job_idx;
    const void              *printer_context = NULL;
    int                     optc;

    /* Process CLI options: */
    while ( (optc = getopt_long(argc, argv, SPART_CLI_OPTSTRING, spart_cli_opts, NULL)) != -1 ) {
        switch ( optc ) {

            case SPART_CLI_OPT_HELP:
                usage(argv[0]);
                exit(0);

            case SPART_CLI_OPT_FORMAT: {
                if ( *optarg ) {
                    if ( strcasecmp(optarg, "text") == 0 ) {
                        printer = &spart_printer_text;
                    }
                    else if ( (strcasecmp(optarg, "parsable") == 0) || (strcasecmp(optarg, "parseable") == 0) ) {
                        printer = &spart_printer_parseable;
                    }
                    else if ( strcasecmp(optarg, "json") == 0 ) {
                        printer = &spart_printer_json;
                    }
                    else if ( strcasecmp(optarg, "yaml") == 0 ) {
                        printer = &spart_printer_yaml;
                    }
                    else {
                        fprintf(stderr, "invalid format: %s\n", optarg);
                        exit(EINVAL);
                    }
                } else {
                    fprintf(stderr, "no format name provided with -f/--format option\n");
                    exit(EINVAL);
                }
                break;
            }

            case SPART_CLI_OPT_PARTITION: {
                if ( *optarg ) {
                    char    *partlist = optarg;

                    /* Might be a comma-separated list, so process it that way: */
                    while ( partlist && *partlist ) {
                        char                *end_ptr = strchr(partlist, ',');
                        spart_string_list_t *new_node = NULL;

                        if ( end_ptr ) {
                            new_node = spart_string_list_add(&selected_partitions, partlist, (end_ptr - partlist));
                            if ( new_node ) partlist = end_ptr + 1;
                        } else {
                            new_node = spart_string_list_add(&selected_partitions, partlist, 0);
                            if ( new_node )partlist = NULL;
                        }
                        if ( ! new_node ) {
                            fprintf(stderr, "failed while adding partition names (at '%s')\n", partlist);
                            exit(EINVAL);
                        }
                    }
                } else {
                    fprintf(stderr, "no partition name provided with -p/--partition option\n");
                    exit(EINVAL);
                }
                break;
            }

        }
    }

    /* Load partition and node records: */
    if ( slurm_load_partitions((time_t) NULL, &part_buffer_ptr, 0) ) {
		fprintf(stderr, "failed to load Slurm partition records (slurm_load_partitions)\n");
		exit(1);
	}
    if ( slurm_load_node((time_t) NULL, &node_buffer_ptr, SHOW_ALL) ) {
		fprintf(stderr, "failed to load Slurm node records (slurm_load_node error)\n");
		exit(1);
	}

    /* Allocate an array of partition summary records: */
    part_data = malloc(part_buffer_ptr->record_count * sizeof(spart_data_t));
	if ( ! part_data ) {
        fprintf(stderr, "failed to allocate partition summary records\n");
        exit(ENOMEM);
    }

    /* Loop over the partitions, initializing each summary record: */
    for ( part_idx = 0; part_idx < part_buffer_ptr->record_count; part_idx++ ) {
        if ( ! spart_data_init(&part_data[part_idx], &part_buffer_ptr->partition_array[part_idx], node_buffer_ptr->node_array) ) {
            fprintf(stderr, "failed to summarize partition\n");
            exit(1);
        }
    }

    /* At this point we're done with the node records:*/
    slurm_free_node_info_msg(node_buffer_ptr);

    /* Load the job records to process them: */
    if ( slurm_load_jobs((time_t) NULL, &job_buffer_ptr, SHOW_ALL) )
	{
		fprintf(stderr, "failed to load Slurm job records (slurm_load_jobs)\n");
		exit(1);
	}

    /* Loop over the jobs to determine which are running versus waiting: */
	for ( job_idx = 0; job_idx < job_buffer_ptr->record_count; job_idx++ ) {
        const char  *req_part = job_buffer_ptr->job_array[job_idx].partition;
        size_t      req_part_len = strlen(req_part);

        /* Loop over the partitions to see which one this job uses: */
        for ( part_idx = 0; part_idx < part_buffer_ptr->record_count; part_idx++ ) {
            /* The job's partition string can actually contain multiple partition names in a
             * comma-separated list, so a simple strncmp won't work; it would also have
             * unpredictable results if there are partition names with overlapping substrings,
             * e.g. for "large" and "large_mpi" a job requesting "large_mpi" would match
             * the "large" partition by strncmp(part_name, job_req_part, len(part_name)) */
            const char      *req_part_scan = req_part;
            size_t          req_part_scan_len = req_part_len;
            const char      *found = NULL;
            unsigned int    part_len = strlen(part_buffer_ptr->partition_array[part_idx].name);

            while ( (req_part_scan_len >= part_len) && (found = strstr(req_part_scan, part_buffer_ptr->partition_array[part_idx].name)) ) {
                /* If the found string:
                 *   - occurs at the start of req_part
                 *   - is preceded by a comma and ends with a comma or NUL
                 * then this partition is selected for the job. */
                if ( (found == req_part) || ((found[part_len] == ',') || (found[part_len] == '\0')) ) break;
                req_part_scan_len -= (found - req_part_scan) + part_len;
                req_part_scan = found + part_len;
                found = NULL;
            }
            if ( found && (job_buffer_ptr->job_array[job_idx].job_state == JOB_PENDING) ) {
                if ( (job_buffer_ptr->job_array[job_idx].state_reason == WAIT_RESOURCES)
                     || (job_buffer_ptr->job_array[job_idx].state_reason == WAIT_PRIORITY)
                ) {
                    part_data[part_idx].pending_cpus_resources += job_buffer_ptr->job_array[job_idx].num_cpus;
                    part_data[part_idx].pending_jobs_resources++;
                } else {
                    part_data[part_idx].pending_cpus_other += job_buffer_ptr->job_array[job_idx].num_cpus;
                    part_data[part_idx].pending_jobs_other++;
				}
			}
		}
	}

    /* Done with the job records */
	slurm_free_job_info_msg(job_buffer_ptr);

    /* At this point we have all the data populated in the part_data array.  Time to display it.
     * Run through the output format pipeline:
     *
     * (1) Allocate contextual storage for the chosen format
     */
    if ( printer->context_alloc_fn ) {
        printer_context = printer->context_alloc_fn(argc, argv);
        if ( ! printer_context ) {
            fprintf(stderr, "unable to allocate storage for printer callback\n");
            exit(ENOMEM);
        }
    }
    /*
     * (2) Pre-process the partition summary list
     */
    if ( printer->preprint_fn ) {
        for ( part_idx = 0; part_idx < part_buffer_ptr->record_count; part_idx++ ) {
            if ( ! printer->preprint_fn(&part_data[part_idx], printer_context) ) {
                fprintf(stderr, "failure while pre-processing the partition print list\n");
                exit(1);
            }
        }
    }
    /*
     * (3) Write header
     */
    if ( printer->header_fn ) {
        printer->header_fn(printer_context);
    }
    /*
     * (4) Write the list of partition summaries
     */
    if ( printer->print_fn ) {
        for ( part_idx = 0; part_idx < part_buffer_ptr->record_count; part_idx++ ) {
            spart_printer_enum_t    enum_flags = 0;

            if ( selected_partitions && ! spart_string_list_find(selected_partitions, part_data[part_idx].partition->name, 0) ) continue;

            if ( part_idx == 0 ) enum_flags |= spart_printer_enum_first;
            if ( part_idx + 1 == part_buffer_ptr->record_count ) enum_flags |= spart_printer_enum_last;

            if ( ! printer->print_fn(&part_data[part_idx], enum_flags, printer_context) ) {
                fprintf(stderr, "failure while printing the partition list\n");
                exit(1);
            }
        }
    }
    /*
     * (5) Write footer
     */
    if ( printer->footer_fn ) {
        printer->footer_fn(printer_context);
    }
    /*
     * (6) Deallocate contextual storage for the chosen format
     */
    if ( printer_context ) {
        if ( printer->context_dealloc_fn ) {
            printer->context_dealloc_fn(printer_context);
        } else {
            free((void*)printer_context);
        }
    }

    /* Release the rest of our allocated memory: */
    free((void*)part_data);
    slurm_free_partition_info_msg (part_buffer_ptr);
    spart_string_list_destroy(&selected_partitions);

    return 0;
}
