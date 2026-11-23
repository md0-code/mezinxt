/* mezinxt.yaml: the template reads as the defaults, a bad file is refused with
 * its line, and settings changed and stored through the comment-keeping editor
 * read back the same from the saved file with the template's comments still
 * there. Writes only under work/tests. MezinXT's own code. */
#include "config.h"
#include "yamledit.h"
#include "mezinxt_default_yaml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int same(const mx_config *a, const mx_config *b) {
    return a->bios == b->bios && a->cpu == b->cpu && a->speed == b->speed && a->ram_kb == b->ram_kb && a->display == b->display &&
           a->look == b->look && a->monitor == b->monitor && a->sound == b->sound && a->volume == b->volume && a->language == b->language &&
           !strcmp(a->drives[0].image, b->drives[0].image) && a->drives[1].write_protect == b->drives[1].write_protect &&
           a->ports[0].type == b->ports[0].type && !strcmp(a->ports[0].path, b->ports[0].path);
}

int main(void) {
    char error[512];
    mx_config defaults, c, back;
    mx_config_defaults(&defaults);

    /* The template is the defaults but for drive A, which has the manual disk beside the file. */
    CHECK(mx_config_parse((const char *)mx_default_yaml, sizeof mx_default_yaml, "dir/mezinxt.yaml", &c, error, sizeof error));
    CHECK(!strcmp(c.drives[0].image, "dir/disks/manual.img"));
    c.drives[0].image[0] = 0;
    c.language = defaults.language;
    CHECK(same(&c, &defaults));

    /* A bad file is refused with its line. */
    static const char bad[] = "machine:\n  ram_kb: 512\n";
    CHECK(!mx_config_parse(bad, sizeof bad - 1, "dir/mezinxt.yaml", &c, error, sizeof error));
    CHECK(strstr(error, "line 2") && strstr(error, "256 or 640"));

    /* Settings changed, stored into the template, saved and loaded back; the comments stay. */
    mx_ydoc doc;
    CHECK(mx_ydoc_parse(&doc, (const char *)mx_default_yaml, sizeof mx_default_yaml));
    c = defaults;
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s/tests/config", MX_WORK);
    snprintf(path, sizeof path, "%s/mezinxt.yaml", dir);
    snprintf(c.source, sizeof c.source, "%s", path);
    c.bios = MX_BIOS_3_0; c.cpu = MX_CPU_8088; c.speed = MX_SPEED_9_54; c.ram_kb = 256; c.display = MX_DISPLAY_MDA;
    c.look = MX_LOOK_PLAIN; c.monitor = MX_MONITOR_RGB; c.sound = 0; c.volume = 45; c.language = MX_LANG_RO;
    snprintf(c.drives[0].image, sizeof c.drives[0].image, "%s/disks/dos.img", dir);
    c.drives[1].write_protect = 1;
    c.ports[0].type = MX_END_SERIAL;
    snprintf(c.ports[0].path, sizeof c.ports[0].path, "COM7");
    CHECK(mx_config_store(&c, &doc));
    size_t length = 0;
    char *text = mx_ydoc_text(&doc, &length);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "# 3.0, 4.0, 5.0 or 5.0-alt") != NULL && strstr(text, "image: disks/dos.img") != NULL);
        free(text);
    }
    FILE *probe = fopen(path, "wb");
    if (!probe) printf("note: %s cannot be written (create %s); file round trip skipped\n", path, dir);
    else {
        fclose(probe);
        CHECK(mx_ydoc_save(&doc, path));
        CHECK(mx_config_load(path, &back, error, sizeof error));
        CHECK(same(&c, &back));
    }
    mx_ydoc_free(&doc);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_config: all passed\n");
    return 0;
}
