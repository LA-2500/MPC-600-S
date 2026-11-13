// Prints every ALSA control on every sound card with its current value. Read only.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define MAX_VALUES 16

static const char *type_name(int type) {
    switch (type) {
    case SNDRV_CTL_ELEM_TYPE_BOOLEAN: return "bool";
    case SNDRV_CTL_ELEM_TYPE_INTEGER: return "int";
    case SNDRV_CTL_ELEM_TYPE_ENUMERATED: return "enum";
    case SNDRV_CTL_ELEM_TYPE_BYTES: return "bytes";
    case SNDRV_CTL_ELEM_TYPE_IEC958: return "iec958";
    case SNDRV_CTL_ELEM_TYPE_INTEGER64: return "int64";
    default: return "?";
    }
}

static void print_enum(int fd, const struct snd_ctl_elem_info *base, unsigned item) {
    struct snd_ctl_elem_info info = *base;
    info.value.enumerated.item = item;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0) printf("'%s'", info.value.enumerated.name);
    else printf("%u", item);
}

static void print_values(int fd, const struct snd_ctl_elem_info *info) {
    if (!(info->access & SNDRV_CTL_ELEM_ACCESS_READ)) {
        printf(" (write only)");
        return;
    }
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id = info->id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) != 0) {
        printf(" (read failed)");
        return;
    }
    unsigned count = info->count < MAX_VALUES ? info->count : MAX_VALUES;
    for (unsigned i = 0; i < count; i++) {
        putchar(i ? ',' : ' ');
        switch (info->type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
        case SNDRV_CTL_ELEM_TYPE_INTEGER:
            printf("%ld", v.value.integer.value[i]);
            break;
        case SNDRV_CTL_ELEM_TYPE_INTEGER64:
            printf("%lld", v.value.integer64.value[i]);
            break;
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
            print_enum(fd, info, v.value.enumerated.item[i]);
            break;
        case SNDRV_CTL_ELEM_TYPE_BYTES:
            printf("%02x", v.value.bytes.data[i]);
            break;
        default:
            printf("-");
            break;
        }
    }
}

static void dump_card(int card) {
    char path[32];
    snprintf(path, sizeof path, "/dev/snd/controlC%d", card);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct snd_ctl_card_info card_info;
    memset(&card_info, 0, sizeof card_info);
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &card_info) == 0)
        printf("## card %d id=%s name=%s\n", card, card_info.id, card_info.name);
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 || list.count == 0) {
        printf("no controls\n");
        close(fd);
        return;
    }
    struct snd_ctl_elem_id *ids = calloc(list.count, sizeof *ids);
    if (ids) {
        list.space = list.count;
        list.pids = ids;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
            for (unsigned i = 0; i < list.used; i++) {
                struct snd_ctl_elem_info info;
                memset(&info, 0, sizeof info);
                info.id = ids[i];
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) != 0) continue;
                printf("numid=%u '%s' idx=%u %s count=%u", ids[i].numid, ids[i].name, ids[i].index, type_name(info.type),
                       info.count);
                if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
                    printf(" range=%ld..%ld", info.value.integer.min, info.value.integer.max);
                printf(" =");
                print_values(fd, &info);
                putchar('\n');
            }
        }
        free(ids);
    }
    close(fd);
}

int main(void) {
    for (int card = 0; card < 8; card++) dump_card(card);
    return 0;
}
