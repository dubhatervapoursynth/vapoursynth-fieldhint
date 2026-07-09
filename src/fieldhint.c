#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth4.h>
#include <VSHelper4.h>


typedef enum {
    HINT_MISSING = -1,
    HINT_NOTCOMBED,
    HINT_COMBED
} hint_t;


typedef struct {
    int tf, bf;
    hint_t hint;
} ovr_t;


typedef struct {
    VSNode *node;
    const VSVideoInfo *vi;

    const char *ovrfile;
    ovr_t *ovr;
    int tff;
    char *matches;
    int num_matches;
} FieldhintData;


static int cmp(const void *av, const void *bv) {
    int a = *(int *)av;
    int b = *(int *)bv;

    if (a < b)
        return -1;
    else if (a == b)
        return 0;
    else if (a > b)
        return 1;

    return 0;
}


static const VSFrame *VS_CC fieldhintGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    FieldhintData *d = (FieldhintData *) instanceData;

    int tf, bf;

    if (d->ovr) {
        tf = d->ovr[n].tf;
        bf = d->ovr[n].bf;
    } else {
        char match = d->matches[n];
        int tff = d->tff;
        if (match == 'n') {
            tf = n + tff;
            bf = n + !tff;
        } else if (match == 'u') {
            tf = n + !tff;
            bf = n + tff;
        } else if (match == 'b') {
            tf = n - !tff;
            bf = n - tff;
        } else if (match == 'p') {
            tf = n - tff;
            bf = n - !tff;
        } else { // 'c' and any invalid characters.
            tf = bf = n;
        }
    }

    if (activationReason == arInitial) {
        int frames[3] = { n, tf, bf };

        qsort(frames, 3, sizeof(int), cmp);

        for (int i = 0; i < 3; i++)
            vsapi->requestFrameFilter(frames[i], d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        VSFrame *frame;
        if (tf == bf) {
            const VSFrame *tmp = vsapi->getFrameFilter(tf, d->node, frameCtx);
            frame = vsapi->copyFrame(tmp, core);
            vsapi->freeFrame(tmp);
        } else {
            frame = vsapi->newVideoFrame(&d->vi->format, d->vi->width, d->vi->height, NULL, core);
            const VSFrame *top = vsapi->getFrameFilter(tf, d->node, frameCtx);
            const VSFrame *bottom = vsapi->getFrameFilter(bf, d->node, frameCtx);

            int plane;

            for (plane = 0; plane < d->vi->format.numPlanes; plane++) {
                uint8_t *dstp = vsapi->getWritePtr(frame, plane);
                ptrdiff_t dst_stride = vsapi->getStride(frame, plane);
                int width = vsapi->getFrameWidth(frame, plane);
                int height = vsapi->getFrameHeight(frame, plane);

                const uint8_t *srcp = vsapi->getReadPtr(top, plane);
                ptrdiff_t src_stride = vsapi->getStride(top, plane);
                vsh_bitblt(dstp, dst_stride*2,
                          srcp, src_stride*2,
                          width*d->vi->format.bytesPerSample, (height+1)/2);

                srcp = vsapi->getReadPtr(bottom, plane);
                src_stride = vsapi->getStride(bottom, plane);
                vsh_bitblt(dstp + dst_stride, dst_stride*2,
                          srcp + src_stride, src_stride*2,
                          width*d->vi->format.bytesPerSample, height/2);
            }

            vsapi->freeFrame(top);
            vsapi->freeFrame(bottom);
        }

        const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        vsapi->clearMap(vsapi->getFramePropertiesRW(frame));
        vsapi->copyMap(vsapi->getFramePropertiesRO(src), vsapi->getFramePropertiesRW(frame));
        vsapi->freeFrame(src);

        if (d->ovr && d->ovr[n].hint != HINT_MISSING) {
            VSMap *props = vsapi->getFramePropertiesRW(frame);
            vsapi->mapSetInt(props, "_Combed", d->ovr[n].hint, maReplace);
        }

        return frame;
    }

    return 0;
}


static void VS_CC fieldhintFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    FieldhintData *d = (FieldhintData *)instanceData;
    vsapi->freeNode(d->node);
    if (d->ovr)
        free(d->ovr);
    if (d->matches)
        free(d->matches);
    free(d);
}


static void VS_CC fieldhintCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    FieldhintData d = { 0 };
    FieldhintData *data;
    int err;

    d.ovrfile = vsapi->mapGetData(in, "ovr", 0, &err);

    const char *matches = vsapi->mapGetData(in, "matches", 0, &err);

    if (!d.ovrfile && !matches) {
        vsapi->mapSetError(out, "FieldHint: Either 'ovr' or 'matches' must be passed.");
        return;
    }

    if (d.ovrfile && matches) {
        vsapi->mapSetError(out, "FieldHint: Only one of 'ovr' and 'matches' must be passed.");
        return;
    }

    d.tff = !!vsapi->mapGetInt(in, "tff", 0, &err);
    if (err && matches) {
        vsapi->mapSetError(out, "FieldHint: 'tff' must be passed when 'matches' is passed.");
        return;
    }

    if (!err && d.ovrfile) {
        vsapi->mapSetError(out, "FieldHint: 'tff' must not be passed when 'ovr' is passed.");
        return;
    }

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = vsapi->getVideoInfo(d.node);

    if (!vsh_isConstantVideoFormat(d.vi)) {
        vsapi->mapSetError(out, "FieldHint: only constant format input supported");
        vsapi->freeNode(d.node);
        return;
    }


    if (d.ovrfile) {
        char buf[80];
        FILE *fh = fopen(d.ovrfile, "r");
        if (!fh) {
            vsapi->freeNode(d.node);
            vsapi->mapSetError(out, "FieldHint: can't open ovr file");
            return;
        }

        // First pass: count the override lines, skipping blank and comment ('#') lines.
        int count = 0;
        while (fgets(buf, sizeof(buf), fh)) {
            int continuation = strchr(buf, '\n') == NULL;
            char *pos = buf + strspn(buf, " \t\r\n");
            if (pos[0] != 0 && pos[0] != '#')
                count++;
            // Consume the remaining chunks of an over-long line so it counts only once.
            while (continuation && fgets(buf, sizeof(buf), fh))
                continuation = strchr(buf, '\n') == NULL;
        }

        d.ovr = malloc(count * sizeof(ovr_t));
        if (count && !d.ovr) {
            fclose(fh);
            vsapi->freeNode(d.node);
            vsapi->mapSetError(out, "FieldHint: failed to allocate overrides.");
            return;
        }

        fseek(fh, 0, SEEK_SET);

        // Second pass: parse the override lines. Blank and comment lines are skipped
        // without consuming an entry, so they never shift the per-frame indices.
        int line = 0;    // physical line number, for error messages
        int frame = 0;   // parsed overrides so far == frame the next override applies to
        while (fgets(buf, sizeof(buf), fh)) {
            line++;
            int continuation = strchr(buf, '\n') == NULL;
            char *pos = buf + strspn(buf, " \t\r\n");

            if (pos[0] != '#' && pos[0] != 0) {
                ovr_t *entry = &d.ovr[frame];
                char hint = 0;
                char error[80];

                if (sscanf(pos, " %d, %d, %c", &entry->tf, &entry->bf, &hint) != 3 &&
                    sscanf(pos, " %d, %d", &entry->tf, &entry->bf) != 2) {
                    fclose(fh);
                    free(d.ovr);
                    vsapi->freeNode(d.node);
                    snprintf(error, sizeof(error), "FieldHint: Can't parse override at line %d", line);
                    vsapi->mapSetError(out, error);
                    return;
                }

                if (entry->tf < 0 || entry->tf >= d.vi->numFrames ||
                    entry->bf < 0 || entry->bf >= d.vi->numFrames) {
                    fclose(fh);
                    free(d.ovr);
                    vsapi->freeNode(d.node);
                    snprintf(error, sizeof(error), "FieldHint: Frame number out of range at line %d", line);
                    vsapi->mapSetError(out, error);
                    return;
                }

                entry->hint = HINT_MISSING;
                if (hint == '-') {
                    entry->hint = HINT_NOTCOMBED;
                } else if (hint == '+') {
                    entry->hint = HINT_COMBED;
                } else if (hint != 0) {
                    fclose(fh);
                    free(d.ovr);
                    vsapi->freeNode(d.node);
                    snprintf(error, sizeof(error), "FieldHint: Invalid combed hint at line %d", line);
                    vsapi->mapSetError(out, error);
                    return;
                }

                frame++;
            }

            // Consume the remaining chunks of an over-long line.
            while (continuation && fgets(buf, sizeof(buf), fh))
                continuation = strchr(buf, '\n') == NULL;
        }

        fclose(fh);

        if (d.vi->numFrames != frame) {
            free(d.ovr);
            vsapi->freeNode(d.node);
            vsapi->mapSetError(out, "FieldHint: The number of overrides and the number of frames don't match.");
            return;
        }
    } else { // No overrides file. Use matches.
        d.num_matches = vsapi->mapGetDataSize(in, "matches", 0, &err);
        if (d.num_matches == 0) {
            vsapi->mapSetError(out, "FieldHint: 'matches' must not be an empty string.");
            vsapi->freeNode(d.node);
            return;
        }

        if (d.vi->numFrames != d.num_matches) {
            vsapi->mapSetError(out, "FieldHint: The number of matches and the number of frames don't match.");
            vsapi->freeNode(d.node);
            return;
        }

        if (matches[0] == 'p' || matches[0] == 'b') {
            vsapi->mapSetError(out, "FieldHint: The first match cannot be 'p' or 'b'.");
            vsapi->freeNode(d.node);
            return;
        }

        if (matches[d.num_matches - 1] == 'n' || matches[d.num_matches - 1] == 'u') {
            vsapi->mapSetError(out, "FieldHint: The last match cannot be 'n' or 'u'.");
            vsapi->freeNode(d.node);
            return;
        }

        d.matches = malloc(d.num_matches + 1);
        memcpy(d.matches, matches, d.num_matches + 1);
    }

    data = malloc(sizeof(d));
    *data = d;

    VSFilterDependency deps[] = { {data->node, rpGeneral} };
    vsapi->createVideoFilter(out, "FieldHint", data->vi, fieldhintGetFrame, fieldhintFree, fmParallel, deps, 1, data, core);
}


VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.nodame.fieldhint", "fh", "FieldHint Plugin", VS_MAKE_VERSION(5, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("FieldHint",
        "clip:vnode;"
        "ovr:data:opt;"
        "tff:int:opt;"
        "matches:data:opt;",
        "clip:vnode;"
        , fieldhintCreate, NULL, plugin);
}
