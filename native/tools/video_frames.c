/* video_frames: saves frames of a video as BMP files, using Windows' own Media Foundation, so
   no extra install is needed. Used to study screen recordings the user sends of the native build.

   video_frames <video> <out folder> <start seconds> <end seconds> <step seconds>
   Writes <out>/f-<milliseconds>.bmp for each step, plus prints the video's size and length. */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <stdio.h>
#include <stdlib.h>

static void write_bmp(const char* path, const BYTE* bgra, LONG stride, UINT32 w, UINT32 h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    UINT32 row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
    BYTE head[54] = {'B', 'M'};
    *(UINT32*)(head + 2) = size;
    *(UINT32*)(head + 10) = 54;
    *(UINT32*)(head + 14) = 40;
    *(INT32*)(head + 18) = (INT32)w;
    *(INT32*)(head + 22) = (INT32)h; /* bottom-up */
    *(UINT16*)(head + 26) = 1;
    *(UINT16*)(head + 28) = 24;
    fwrite(head, 1, 54, f);
    BYTE* line = malloc(row + pad);
    memset(line, 0, row + pad);
    for (UINT32 y = 0; y < h; y++) {
        const BYTE* src = bgra + (LONG)(h - 1 - y) * stride;
        for (UINT32 x = 0; x < w; x++) memcpy(line + x * 3, src + x * 4, 3);
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "video_frames <video> <out folder> <start s> <end s> <step s>\n");
        return 1;
    }
    double start = atof(argv[3]), end = atof(argv[4]), step = atof(argv[5]);
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
    CreateDirectoryA(argv[2], NULL);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    IMFAttributes* attrs = NULL;
    MFCreateAttributes(&attrs, 1);
    IMFAttributes_SetUINT32(attrs, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* reader = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(path, attrs, &reader);
    if (FAILED(hr)) {
        fprintf(stderr, "open failed %08lX\n", hr);
        return 1;
    }
    IMFMediaType* want = NULL;
    MFCreateMediaType(&want);
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want);
    if (FAILED(hr)) {
        fprintf(stderr, "RGB32 output failed %08lX\n", hr);
        return 1;
    }
    IMFMediaType* got = NULL;
    IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got);
    UINT64 packed = 0;
    IMFMediaType_GetUINT64(got, &MF_MT_FRAME_SIZE, &packed);
    UINT32 w = (UINT32)(packed >> 32), h = (UINT32)packed;
    UINT64 rate = 0;
    IMFMediaType_GetUINT64(got, &MF_MT_FRAME_RATE, &rate);
    PROPVARIANT length;
    PropVariantInit(&length);
    IMFSourceReader_GetPresentationAttribute(reader, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &length);
    GUID sub;
    IMFMediaType_GetGUID(got, &MF_MT_SUBTYPE, &sub);
    UINT32 default_stride = 0;
    IMFMediaType_GetUINT32(got, &MF_MT_DEFAULT_STRIDE, &default_stride);
    printf("subtype %08lX stride %d\n", sub.Data1, (int)default_stride);
    printf("size %ux%u rate %u/%u length %.2f s\n", w, h, (UINT32)(rate >> 32), (UINT32)rate,
           length.uhVal.QuadPart / 1e7);

    for (double t = start; t <= end + 1e-6; t += step) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = (LONGLONG)(t * 1e7);
        IMFSourceReader_SetCurrentPosition(reader, &GUID_NULL, &pos);
        /* Seeking lands on the key frame before t, so read forward to the first frame at or after t. */
        for (;;) {
            DWORD flags = 0;
            LONGLONG stamp = 0;
            IMFSample* sample = NULL;
            hr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags,
                                            &stamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) goto done;
            if (!sample) continue;
            if (stamp + 1 < (LONGLONG)(t * 1e7) - 170000) { /* more than one frame early */
                IMFSample_Release(sample);
                continue;
            }
            IMFMediaBuffer* buffer = NULL;
            IMFSample_ConvertToContiguousBuffer(sample, &buffer);
            BYTE* data = NULL;
            DWORD size = 0;
            IMFMediaBuffer_Lock(buffer, &data, NULL, &size);
            LONG stride = default_stride ? (LONG)(INT32)default_stride : (LONG)(w * 4);
            if (stride < 0) stride = -stride;
            if (t == start) printf("buffer %lu bytes, stride %ld\n", size, stride);
            char name[MAX_PATH];
            snprintf(name, sizeof name, "%s/f-%07lld.bmp", argv[2], stamp / 10000);
            write_bmp(name, data, stride, w, h);
            IMFMediaBuffer_Unlock(buffer);
            IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
            break;
        }
    }
done:
    printf("done\n");
    return 0;
}
