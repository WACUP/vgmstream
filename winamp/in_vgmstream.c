/**
 * vgmstream for Winamp
 */

/* Normally Winamp opens unicode files by their DOS 8.3 name. #define this to use wchar_t filenames,
 * which must be opened with _wfopen in a WINAMP_STREAMFILE (needed for dual files like .pos).
 * Only for Winamp paths, other parts would need #define UNICODE for Windows. */
#ifdef VGM_WINAMP_UNICODE
#define UNICODE_INPUT_PLUGIN
#endif

#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif

#define SKIP_INT_DEFINES

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <io.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <delayimp.h>

#include "../src/vgmstream.h"
#include "../src/util.h"
#include <sdk/winamp/in2.h>
#include <sdk/winamp/wa_ipc.h>
#include <sdk/winamp/ipc_pe.h>
#include <sdk/nu/ServiceBuilder.h>

#include <api/service/api_service.h>
extern api_service *serviceManager;
#define WASABI_API_SVC serviceManager

#include <api/service/waservicefactory.h>

#include <sdk/Agave/Config/api_config.h>
extern api_config *configApi;
#define AGAVE_API_CONFIG configApi

#include <sdk/Agave/Language/api_language.h>

#include <loader/loader/utils.h>
#include <loader/loader/paths.h>

#include "resource.h"

#ifndef VERSION
#define VERSION "2.0"
#endif

#ifndef VERSIONW
#define VERSIONW L"2.0"
#endif

#define LIBVGMSTREAM_BUILD "1050-1102-g95d9a7f5-wacup"
#define APP_NAME "vgmstream plugin"
#define PLUGIN_DESCRIPTION "vgmstream Decoder v" VERSION
#define PLUGIN_DESCRIPTIONW L"vgmstream Decoder v" VERSIONW
#define INI_NAME "plugin.ini"

/* post when playback stops */
#define WM_WA_MPEG_EOF WM_USER+2

extern In_Module plugin; /* the input module, declared at the bottom of this file */
DWORD WINAPI __stdcall decode(void *arg);

/* Winamp Play extension list, needed to accept/play and associate extensions in Windows */
#define EXTENSION_LIST_SIZE   (0x2000 * 6)
#define EXT_BUFFER_SIZE 200
char working_extension_list[EXTENSION_LIST_SIZE] = {0};

api_service* WASABI_API_SVC = NULL;
api_language* WASABI_API_LNG = NULL;
api_config *AGAVE_API_CONFIG = NULL;

wchar_t plugindir[MAX_PATH] = {0};

#define FADE_SECONDS_INI_ENTRY TEXT("fade_seconds")
#define FADE_DELAY_SECONDS_INI_ENTRY TEXT("fade_delay")
#define LOOP_COUNT_INI_ENTRY TEXT("loop_count")
#define LOOP_FOREVER_INI_ENTRY TEXT("loop_forever")
#define IGNORE_LOOP_INI_ENTRY TEXT("ignore_loop")
#define DISABLE_SUBSONGS_INI_ENTRY TEXT("disable_subsongs")
#define DOWNMIX_INI_ENTRY TEXT("downmix")

double fade_seconds = 10.0;
double fade_delay_seconds = 0.0;
double loop_count = 2.0;
int loop_forever = 0;
int ignore_loop = 0;
int disable_subsongs = 0;
int downmix = 0;
int loaded_config = 0;

// {B6CB4A7C-A8D0-4c55-8E60-9F7A7A23DA0F}
static const GUID playbackConfigGroupGUID = 
{ 0xb6cb4a7c, 0xa8d0, 0x4c55, { 0x8e, 0x60, 0x9f, 0x7a, 0x7a, 0x23, 0xda, 0xf } };


/* plugin state */
VGMSTREAM * vgmstream = NULL;
HANDLE decode_thread_handle = INVALID_HANDLE_VALUE;
short sample_buffer[(576*2) * 2] = {0}; /* at least 576 16-bit samples, stereo, doubled in case Winamp's DSP is active */

int paused = 0;
int decode_abort = 0;
int seek_needed_samples = -1;
int decode_pos_ms = 0;
int decode_pos_samples = 0;
int stream_length_samples = 0;
int fade_samples = 0;
int output_channels = 0;

wchar_t lastfn[MAX_PATH] = {0}; /* name of the currently playing file */

/* ************************************* */

/* converts from utf16 to utf8 (if unicode is active) */
static void wa_wchar_to_char(char *dst, size_t dstsize, const in_char *wsrc) {
    /* converto to UTF8 codepage, default separate bytes, source wstr, wstr lenght,  */
    //int size_needed = WideCharToMultiByte(CP_UTF8,0, src,-1, NULL,0, NULL, NULL);
    WideCharToMultiByte(CP_UTF8,0, wsrc,-1, dst,dstsize, NULL, NULL);
}

/* converts from utf8 to utf16 (if unicode is active) */
static void wa_char_to_wchar(in_char *wdst, size_t wdstsize, const char *src) {
    //int size_needed = MultiByteToWideChar(CP_UTF8,0, src,-1, NULL,0);
    MultiByteToWideChar(CP_UTF8,0, src,-1, wdst,wdstsize);
}

/* opens a utf16 (unicode) path */
static FILE* wa_fopen(const in_char *wpath) {
    return _wfopen(wpath,L"rb");
}

/* dupes a utf16 (unicode) file */
static FILE* wa_fdopen(int fd) {
    return _wfdopen(fd,L"rb");
}

/* ************************************* */
/* IN_STREAMFILE                         */
/* ************************************* */

/* a STREAMFILE that operates via STDIOSTREAMFILE but handles Winamp's unicode (in_char) paths */
typedef struct {
    STREAMFILE sf;
    STREAMFILE *stdiosf;
    FILE *infile_ref; /* pointer to the infile in stdiosf */
} WINAMP_STREAMFILE;

static STREAMFILE *open_winamp_streamfile_by_file(FILE *infile, const char * path);
static STREAMFILE *open_winamp_streamfile_by_wpath(const in_char *wpath);

static size_t wasf_read(WINAMP_STREAMFILE *streamfile, uint8_t *dest, off_t offset, size_t length) {
    return streamfile->stdiosf->read(streamfile->stdiosf,dest,offset,length);
}

static off_t wasf_get_size(WINAMP_STREAMFILE *streamfile) {
    return streamfile->stdiosf->get_size(streamfile->stdiosf);
}

static off_t wasf_get_offset(WINAMP_STREAMFILE *streamfile) {
    return streamfile->stdiosf->get_offset(streamfile->stdiosf);
}

static void wasf_get_name(WINAMP_STREAMFILE *streamfile, char *buffer, size_t length) {
    streamfile->stdiosf->get_name(streamfile->stdiosf, buffer, length);
}

static void wasf_get_realname(WINAMP_STREAMFILE *streamfile, char *buffer, size_t length) {
    streamfile->stdiosf->get_realname(streamfile->stdiosf, buffer, length);
}

static STREAMFILE *wasf_open(WINAMP_STREAMFILE *streamFile, const char *const filename, size_t buffersize) {
    int newfd;
    FILE *newfile;
    STREAMFILE *newstreamFile;
	in_char wpath[PATH_LIMIT] = {0};
    char name[PATH_LIMIT] = {0};

    if (!filename)
        return NULL;

    /* if same name, duplicate the file pointer we already have open */ //unsure if all this is needed
    streamFile->stdiosf->get_name(streamFile->stdiosf, name, PATH_LIMIT);
    if (!strcmp(name,filename)) {
        if (((newfd = dup(fileno(streamFile->infile_ref))) >= 0) &&
            (newfile = wa_fdopen(newfd)))
        {
            newstreamFile = open_winamp_streamfile_by_file(newfile,filename);
            if (newstreamFile) {
                return newstreamFile;
            }
            // failure, close it and try the default path (which will probably fail a second time)
            fclose(newfile);
        }
    }

    /* STREAMFILEs carry char/UTF8 names, convert to wchar for Winamp */
    wa_char_to_wchar(wpath, PATH_LIMIT, filename);
    return open_winamp_streamfile_by_wpath(wpath);
}

static void wasf_close(WINAMP_STREAMFILE *streamfile) {
    /* closes infile_ref + frees in the internal STDIOSTREAMFILE (fclose for wchar is not needed) */
    streamfile->stdiosf->close(streamfile->stdiosf);
    free(streamfile); /* and the current struct */
}

static STREAMFILE *open_winamp_streamfile_by_file(FILE *infile, const char * path) {
    WINAMP_STREAMFILE *this_sf = NULL;
    STREAMFILE *stdiosf = NULL;

    this_sf = (WINAMP_STREAMFILE*)calloc(1,sizeof(WINAMP_STREAMFILE));
    if (!this_sf) goto fail;

    stdiosf = open_stdio_streamfile_by_file(infile,path);
    if (!stdiosf) goto fail;

    this_sf->sf.read = (size_t (*)(struct _STREAMFILE *,uint8_t * dest, off_t offset, size_t length))wasf_read;
    this_sf->sf.get_size = (size_t (*)(struct _STREAMFILE *))wasf_get_size;
    this_sf->sf.get_offset = (off_t (*)(struct _STREAMFILE *))wasf_get_offset;
    this_sf->sf.get_name = (void (*)(struct _STREAMFILE *,char *name,size_t length))wasf_get_name;
    this_sf->sf.get_realname = (void (*)(struct _STREAMFILE *,char *name,size_t length))wasf_get_realname;
    this_sf->sf.open = (struct _STREAMFILE * (*)(struct _STREAMFILE *,const char * const filename,size_t buffersize))wasf_open;
    this_sf->sf.close = (void (*)(struct _STREAMFILE *))wasf_close;

    this_sf->stdiosf = stdiosf;
    this_sf->infile_ref = infile;

    return &this_sf->sf; /* pointer to STREAMFILE start = rest of the custom data follows */

fail:
    close_streamfile(stdiosf);
    free(this_sf);
    return NULL;
}


static STREAMFILE *open_winamp_streamfile_by_wpath(const in_char *wpath) {
    FILE *infile = NULL;
    STREAMFILE *streamFile;
    char path[PATH_LIMIT] = {0};

    /* open a FILE from a Winamp (possibly UTF-16) path */
    infile = wa_fopen(wpath);
    if (!infile) return NULL;

    /* convert to UTF-8 if needed for internal use */
    wa_wchar_to_char(path, PATH_LIMIT, wpath);

    streamFile = open_winamp_streamfile_by_file(infile, path);
    if (!streamFile) {
        fclose(infile);
    }

    return streamFile;
}

/* opens vgmstream for winamp */
static VGMSTREAM* init_vgmstream_winamp(const in_char *fn, int stream_index) {
    VGMSTREAM * vgmstream = NULL;

    //return init_vgmstream(fn);

    /* manually init streamfile to pass the stream index */
    STREAMFILE *streamFile = open_winamp_streamfile_by_wpath(fn); //open_stdio_streamfile(fn);
    if (streamFile) {
        streamFile->stream_index = stream_index;
        vgmstream = init_vgmstream_from_STREAMFILE(streamFile);
        close_streamfile(streamFile);
    }

    return vgmstream;
}


/* ************************************* */
/* IN_CONFIG                             */
/* ************************************* */

#ifndef UNICODE_INPUT_PLUGIN
/* converts from utf8 to utf16 (if unicode is active) */
static void cfg_char_to_wchar(TCHAR *wdst, size_t wdstsize, const char *src) {
#ifdef UNICODE
    //int size_needed = MultiByteToWideChar(CP_UTF8,0, src,-1, NULL,0);
    MultiByteToWideChar(CP_UTF8,0, src,-1, wdst,wdstsize);
#else
    strcpy(wdst,src);
#endif
}
#endif

/* config */
#define CONFIG_APP_NAME TEXT("vgmstream plugin")
#define CONFIG_INI_NAME TEXT("plugin.ini")

#define DEFAULT_FADE_SECONDS TEXT("10.00")
#define DEFAULT_FADE_DELAY_SECONDS TEXT("0.00")
#define DEFAULT_LOOP_COUNT TEXT("2.00")
#define DEFAULT_LOOP_FOREVER 0
#define DEFAULT_IGNORE_LOOP 0
#define DEFAULT_DISABLE_SUBSONGS 0
#define DEFAULT_DOWNMIX 0

void read_config() {
	if (!loaded_config) {
		wchar_t buf[256] = {0};
		int consumed;

		loaded_config = 1;

		GetPrivateProfileString(CONFIG_APP_NAME, FADE_SECONDS_INI_ENTRY, DEFAULT_FADE_SECONDS, buf, ARRAYSIZE(buf), get_paths()->plugin_ini_file);
		if (swscanf(buf, L"%lf%n", &fade_seconds, &consumed) < 1 || consumed != wcslen(buf) || fade_seconds < 0) {
			(void)swscanf(DEFAULT_FADE_SECONDS, L"%lf", &fade_seconds);
		}

		GetPrivateProfileString(CONFIG_APP_NAME, FADE_DELAY_SECONDS_INI_ENTRY, DEFAULT_FADE_DELAY_SECONDS, buf, ARRAYSIZE(buf), get_paths()->plugin_ini_file);
		if (swscanf(buf, L"%lf%n", &fade_delay_seconds, &consumed) < 1 || consumed != wcslen(buf)) {
			(void)swscanf(DEFAULT_FADE_DELAY_SECONDS, L"%lf", &fade_delay_seconds);
		}

		GetPrivateProfileString(CONFIG_APP_NAME, LOOP_COUNT_INI_ENTRY, DEFAULT_LOOP_COUNT, buf, ARRAYSIZE(buf), get_paths()->plugin_ini_file);
		if (swscanf(buf, L"%lf%n", &loop_count, &consumed) != 1 || consumed != wcslen(buf) || loop_count < 0) {
			(void)swscanf(DEFAULT_LOOP_COUNT, L"%lf", &loop_count);
		}

		loop_forever = GetPrivateProfileInt(CONFIG_APP_NAME, LOOP_FOREVER_INI_ENTRY, DEFAULT_LOOP_FOREVER, get_paths()->plugin_ini_file);
		ignore_loop = GetPrivateProfileInt(CONFIG_APP_NAME, IGNORE_LOOP_INI_ENTRY, DEFAULT_IGNORE_LOOP, get_paths()->plugin_ini_file);

		if (loop_forever && ignore_loop) {
			_snwprintf(buf, ARRAYSIZE(buf), L"%d", DEFAULT_LOOP_FOREVER);
			loop_forever = DEFAULT_LOOP_FOREVER;

			_snwprintf(buf, ARRAYSIZE(buf), L"%d", DEFAULT_IGNORE_LOOP);
			ignore_loop = DEFAULT_IGNORE_LOOP;
		}
	}
}

/* config dialog handler */
INT_PTR CALLBACK configDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) { 
        case WM_CLOSE:
			{
				EndDialog(hDlg,TRUE);
				return TRUE;
			}
        case WM_INITDIALOG:
			{
				wchar_t buf[256] = {0};
				read_config();

				_snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_seconds);
				SetDlgItemText(hDlg, IDC_FADE_SECONDS, buf);

				_snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_delay_seconds);
				SetDlgItemText(hDlg, IDC_FADE_DELAY_SECONDS, buf);

				_snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", loop_count);
				SetDlgItemText(hDlg, IDC_LOOP_COUNT, buf);

				if (loop_forever) {
					CheckDlgButton(hDlg, IDC_LOOP_FOREVER, BST_CHECKED);
				}
				else if (ignore_loop) {
					CheckDlgButton(hDlg, IDC_IGNORE_LOOP, BST_CHECKED);
				}
				else {
					CheckDlgButton(hDlg, IDC_LOOP_NORMALLY, BST_CHECKED);
				}

				if (disable_subsongs)
					CheckDlgButton(hDlg, IDC_DISABLE_SUBSONGS, BST_CHECKED);

				if (downmix)
					CheckDlgButton(hDlg, IDC_DOWNMIX, BST_CHECKED);
			}
            break;
        case WM_COMMAND:
            switch (GET_WM_COMMAND_ID(wParam, lParam)) {
                case IDOK:
                    {
						wchar_t buf[256] = {0};
                        double temp_fade_seconds = 10.0;
                        double temp_fade_delay_seconds = 0.0;
                        double temp_loop_count = 2.0;
                        int consumed;

                        /* read and verify */
                        GetDlgItemText(hDlg, IDC_FADE_SECONDS, buf, ARRAYSIZE(buf));
                        if (swscanf(buf, L"%lf%n", &temp_fade_seconds, &consumed) < 1
                            || consumed != wcslen(buf) || temp_fade_seconds < 0) {
                            MessageBox(hDlg, L"Invalid value for Fade Length\n"
									   L"Must be a number greater than or equal to zero",
									   L"Error", MB_OK | MB_ICONERROR);
                            break;
                        }

                        GetDlgItemText(hDlg, IDC_FADE_DELAY_SECONDS, buf, ARRAYSIZE(buf));
                        if (swscanf(buf, L"%lf%n", &temp_fade_delay_seconds,
                            &consumed) < 1 || consumed != wcslen(buf)) {
                            MessageBox(hDlg, L"Invalid valid for Fade Delay\n"
									   L"Must be a number", L"Error", MB_OK | MB_ICONERROR);
                            break;
                        }

                        GetDlgItemText(hDlg, IDC_LOOP_COUNT, buf, ARRAYSIZE(buf));
                        if (swscanf(buf, L"%lf%n", &temp_loop_count, &consumed) < 1 ||
                            consumed != wcslen(buf) || temp_loop_count < 0) {
                            MessageBox(hDlg, L"Invalid value for Loop Count\n"
									   L"Must be a number greater than or equal to zero",
									   L"Error", MB_OK | MB_ICONERROR);
                            break;
                        }

                        fade_seconds = temp_fade_seconds;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_seconds);
						WritePrivateProfileString(CONFIG_APP_NAME, FADE_SECONDS_INI_ENTRY, buf, get_paths()->plugin_ini_file);

                        fade_delay_seconds = temp_fade_delay_seconds;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_delay_seconds);
						WritePrivateProfileString(CONFIG_APP_NAME, FADE_DELAY_SECONDS_INI_ENTRY, buf, get_paths()->plugin_ini_file);

                        loop_count = temp_loop_count;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", loop_count);
						WritePrivateProfileString(CONFIG_APP_NAME, LOOP_COUNT_INI_ENTRY, buf, get_paths()->plugin_ini_file);
                    }
					/* pass through */
                case IDCANCEL:
					{
						EndDialog(hDlg,TRUE);
						break;
					}
				case IDC_LOOP_NORMALLY:
				case IDC_LOOP_FOREVER:
				case IDC_IGNORE_LOOP:
					{
						wchar_t buf[256] = {0};
                        loop_forever = (IsDlgButtonChecked(hDlg, IDC_LOOP_FOREVER) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", loop_forever);
						WritePrivateProfileString(CONFIG_APP_NAME, LOOP_FOREVER_INI_ENTRY, buf, get_paths()->plugin_ini_file);

                        ignore_loop = (IsDlgButtonChecked(hDlg, IDC_IGNORE_LOOP) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", ignore_loop);
						WritePrivateProfileString(CONFIG_APP_NAME, IGNORE_LOOP_INI_ENTRY, buf, get_paths()->plugin_ini_file);
						break;
					}
				case IDC_DISABLE_SUBSONGS:
					{
						wchar_t buf[256] = {0};
						disable_subsongs = (IsDlgButtonChecked(hDlg, IDC_DISABLE_SUBSONGS) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", disable_subsongs);
						WritePrivateProfileString(CONFIG_APP_NAME, DISABLE_SUBSONGS_INI_ENTRY, buf, get_paths()->plugin_ini_file);
						break;
					}
				case IDC_DOWNMIX:
					{
						wchar_t buf[256] = {0};
						downmix = (IsDlgButtonChecked(hDlg, IDC_DOWNMIX) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", downmix);
						WritePrivateProfileString(CONFIG_APP_NAME, DOWNMIX_INI_ENTRY, buf, get_paths()->plugin_ini_file);
						break;
					}
                case IDC_DEFAULT_BUTTON:
					{
						SetDlgItemText(hDlg, IDC_FADE_SECONDS, DEFAULT_FADE_SECONDS);
						SetDlgItemText(hDlg, IDC_FADE_DELAY_SECONDS, DEFAULT_FADE_DELAY_SECONDS);
						SetDlgItemText(hDlg, IDC_LOOP_COUNT, DEFAULT_LOOP_COUNT);

						CheckDlgButton(hDlg, IDC_LOOP_FOREVER, BST_UNCHECKED);
						CheckDlgButton(hDlg, IDC_IGNORE_LOOP, BST_UNCHECKED);
						CheckDlgButton(hDlg, IDC_LOOP_NORMALLY, BST_CHECKED);

						CheckDlgButton(hDlg, IDC_DISABLE_SUBSONGS, BST_UNCHECKED);
						CheckDlgButton(hDlg, IDC_DOWNMIX, BST_UNCHECKED);

						// physically remove the section from the ini file as it's quicker
						WritePrivateProfileString(CONFIG_APP_NAME, 0, 0, get_paths()->plugin_ini_file);
						break;
					}
                default:
                    return FALSE;
            }
        default:
            return FALSE;
    }

    return TRUE;
}


/* ***************************************** */
/* IN_VGMSTREAM UTILS                        */
/* ***************************************** */

/* makes a modified filename, suitable to pass parameters around */
static void make_fn_subsong(in_char * dst, int dst_size, const in_char * filename, int stream_index) {
    /* Follows "(file)(config)(ext)". Winamp needs to "see" (ext) to validate, and file goes first so relative
     * files work in M3Us (path is added). Protocols a la "vgmstream://(config)(file)" work but don't get full paths. */
    _snwprintf(dst, dst_size, L"%s|$s=%i|.vgmstream", filename, stream_index);
}

/* unpacks the subsongs by adding entries to the playlist */
static int split_subsongs(const in_char * filename, int stream_index, VGMSTREAM *vgmstream) {
    int i, playlist_index;
    HWND hPlaylistWindow;

    if (disable_subsongs || vgmstream->num_streams <= 1 || (vgmstream->num_streams > 1 && stream_index > 0))
        return 0; /* no split if no subsongs or playing a subsong */

    hPlaylistWindow = (HWND)SendMessage(plugin.hMainWindow, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND);
    playlist_index = SendMessage(plugin.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTPOS);

    /* The only way to pass info around in Winamp is encoding it into the filename, so a fake name
     * is created with the index. Then, winamp_Play (and related) intercepts and reads the index. */
    for (i = 0; i < vgmstream->num_streams; i++) {
        in_char stream_fn[PATH_LIMIT] = {0};

        make_fn_subsong(stream_fn, PATH_LIMIT, filename, (i + 1)); /* encode index in filename */

        /* insert at index */
        {
            COPYDATASTRUCT cds = {0};
            fileinfoW f;

            wcsncpy(f.file, stream_fn, MAX_PATH - 1);
            f.file[MAX_PATH-1] = '\0';
            f.index = playlist_index + (i + 1);
            cds.dwData = IPC_PE_INSERTFILENAMEW;
            cds.lpData = (void*)&f;
            cds.cbData = sizeof(fileinfoW);
            SendMessage(hPlaylistWindow, WM_COPYDATA, 0, (LPARAM)&cds);
        }
        /* IPC_ENQUEUEFILE can pre-set the title without needing the Playlist handle, but can't insert at index */
    }

    /* remove current file from the playlist */
    SendMessage(hPlaylistWindow, WM_WA_IPC, IPC_PE_DELETEINDEX, playlist_index);

    /* autoplay doesn't always advance to the first unpacked track, manually fails too */
    //SendMessage(input_module.hMainWindow,WM_WA_IPC,playlist_index,IPC_SETPLAYLISTPOS);
    //SendMessage(input_module.hMainWindow,WM_WA_IPC,0,IPC_STARTPLAY);
    return 1;
}

/* parses a modified filename ('fakename') extracting tags parameters (NULL tag for first = filename) */
static int parse_fn_string(const in_char * fn, const in_char * tag, in_char * dst, int dst_size) {
    const in_char *end = wcschr(fn,'|');
    if (tag == NULL) {
        wcscpy(dst, fn);
        if (end)
            dst[end - fn] = '\0';
        return 1;
    }

    //todo actually find + read tags
    dst[0] = '\0';
    return 0;
}

static int parse_fn_int(const in_char * fn, const in_char * tag, int * num) {
    const in_char * start = wcschr(fn,'|');

    //todo actually find + read tags
    if (start > 0) {
        swscanf(start + 1, L"$s=%i ", num);
        return 1;
    } else {
        *num = 0;
        return 0;
    }
}

/* Adds ext to Winamp's extension list */
static void add_extension(int length, char * dst, const char * ext) {
    char buf[EXT_BUFFER_SIZE] = {0};
    char ext_upp[EXT_BUFFER_SIZE] = {0};
    int ext_len, written;
    int i, j;
    if (length <= 1)
        return;

    ext_len = strlen(ext);

    /* find end of dst (double \0), saved in i */
    for (i = 0; i < length - 2 && (dst[i] || dst[i + 1]); i++)
        ;

    /* check if end reached or not enough room to add */
    if (i == length-2 || i + EXT_BUFFER_SIZE+2 > length-2 || ext_len * 3 + 20+2 > EXT_BUFFER_SIZE) {
        dst[i] = '\0';
        dst[i + 1] = '\0';
        return;
    }

    if (i > 0)
        ++i;

    /* uppercase ext */
    for (j = 0; j < ext_len; j++)
        ext_upp[j] = toupper(ext[j]);
    ext_upp[j] = '\0';

    /* copy new extension + double null terminate */
	/*ex: "vgmstream\0vgmstream Audio File (*.VGMSTREAM)\0" */
    written = sprintf(buf, "%s%c%s Audio File (*.%s)%c", ext,'\0',ext_upp,ext_upp,'\0');
    for (j = 0; j < written; i++,j++)
        dst[i] = buf[j];
    dst[i] = '\0';
    dst[i + 1] = '\0';
}

/* Creates Winamp's extension list, a single string that ends with \0\0.
 * Each extension must be in this format: "extension\0Description\0" */
static void build_extension_list() {
    size_t ext_list_len = 0, i;
    const char ** ext_list = vgmstream_get_formats(&ext_list_len);

    for (i = 0; i < ext_list_len; i++) {
        add_extension(EXTENSION_LIST_SIZE, working_extension_list, ext_list[i]);
    }
}

/* unicode utils */
static void get_title(in_char * dst, int dst_size, const in_char * fn, VGMSTREAM * infostream) {
    in_char *basename;
	in_char buffer[PATH_LIMIT] = {0};
    in_char filename[PATH_LIMIT] = {0};
    int stream_index = 0;

    parse_fn_string(fn, NULL, filename, PATH_LIMIT);
    parse_fn_int(fn, L"$s", &stream_index);

    basename = (in_char*)filename + wcslen(filename); /* find end */
    while (*basename != '\\' && basename >= filename) /* and find last "\" */
        --basename;
    ++basename;
    wcscpy(dst, basename);

    /* show stream subsong number */
    if (stream_index > 0) {
        _snwprintf(buffer, PATH_LIMIT, L"#%i", stream_index);
        wcscat(dst, buffer);
    }

    /* show name, but not for the base stream */
    if (infostream && infostream->stream_name[0] != '\0' && stream_index > 0) {
        in_char stream_name[PATH_LIMIT] = {0};
        wa_char_to_wchar(stream_name, PATH_LIMIT, infostream->stream_name);
        _snwprintf(buffer, PATH_LIMIT, L" (%s)", stream_name);
        wcscat(dst, buffer);
    }
}


/* ***************************************** */
/* IN_VGMSTREAM                              */
/* ***************************************** */

void about(HWND hwndParent)
{
	// TODO need to ensure that we keep the build # correct
    AboutMessageBox(hwndParent, PLUGIN_DESCRIPTIONW L"\n\n"
					L"Copyright © 2008-2018 hcs, FastElbja, manakoAT, bxaimc,\n"
					L"snakemeat, soneek, kode54, bnnm and all other contributors.\n\n"
					L"Winamp integration updates by Darren Owen aka DrO\n\n"
					L"Build date: " TEXT(__DATE__) L"\n\n"
					L"Using libvgmstream build: " TEXT(LIBVGMSTREAM_BUILD) L"\n\n\n"
					L"This adds support for dozens of streamed ADPCM and other\n"
					L"formats extracted from various console and PC video games.\n\n"
					L"See https://hcs64.com/vgmstream.html for more information\n"
					L"about the project, source code and alternative versions, etc.",
					L"vgmstream Decoder");
}

/* called at program init */
int init() {

	// load all of the required wasabi services from the winamp client
	ServiceBuild(plugin.service, AGAVE_API_CONFIG, AgaveConfigGUID);
	ServiceBuild(plugin.service, WASABI_API_LNG, languageApiGUID);

	// need to have this initialised before we try to do anything with localisation features
	/*WASABI_API_START_LANG(plugin.hDllInstance, InWvLangGuid);

	StringCchPrintf(pluginTitle, ARRAYSIZE(pluginTitle), WASABI_API_LNGSTRINGW(IDS_PLUGIN_NAME), PLUGIN_VERSION);
	plugin.description = (char*)pluginTitle;*/

    /* dynamically make a list of supported extensions */
    build_extension_list();
	return IN_INIT_SUCCESS;
}

/* called at program quit */
void quit() {
    ServiceRelease(plugin.service, AGAVE_API_CONFIG, AgaveConfigGUID);
    ServiceRelease(plugin.service, WASABI_API_LNG, languageApiGUID);
}

/* called before extension checks, to allow detection of mms://, etc */
int isourfile(const in_char *fn) {
    return 0;
}

/* request to start playing a file */
int play(const in_char *fn) {
    int max_latency;
    in_char filename[PATH_LIMIT] = {0};
    int stream_index = 0;

	read_config();

    if (vgmstream)
        return 1; // TODO: this should either pop up an error box or close the file

    /* check for info encoded in the filename */
    parse_fn_string(fn, NULL, filename, PATH_LIMIT);
    parse_fn_int(fn, L"$s", &stream_index);

    /* open the stream */
    vgmstream = init_vgmstream_winamp(filename, stream_index);
    if (!vgmstream)
        return 1;

    /* add N subsongs to the playlist, if any */
    if (split_subsongs(filename, stream_index, vgmstream)) {
        close_vgmstream(vgmstream);
        vgmstream = NULL;
        return 1;
    }

    /* config */
    if (ignore_loop)
        vgmstream->loop_flag = 0;

    output_channels = vgmstream->channels;
    if (downmix)
        output_channels = vgmstream->channels > 2 ? 2 : vgmstream->channels;


    /* save original name */
    wcsncpy(lastfn, fn, PATH_LIMIT);

    /* open the output plugin */
    max_latency = plugin.outMod->Open(vgmstream->sample_rate, output_channels, 16, 0, 0);

    if (max_latency < 0) {
        close_vgmstream(vgmstream);
        vgmstream = NULL;
        return 1;
    }

    /* set info display */
    plugin.SetInfo(get_vgmstream_average_bitrate(vgmstream) / 1000, vgmstream->sample_rate / 1000, output_channels, 1);

    /* setup visualization */
    plugin.SAVSAInit(max_latency, vgmstream->sample_rate);
    plugin.VSASetInfo(vgmstream->sample_rate, output_channels);

    /* reset internals */
    decode_abort = 0;
    seek_needed_samples = -1;
    decode_pos_ms = 0;
    decode_pos_samples = 0;
    paused = 0;
    stream_length_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, vgmstream);

    fade_samples = (int)(fade_seconds * vgmstream->sample_rate);

    /* start */
    decode_thread_handle = CreateThread(NULL,   /* handle cannot be inherited */
										0,      /* stack size, 0=default */
										decode, /* thread start routine */
										NULL,   /* no parameter to start routine */
										0,      /* run thread immediately */
										NULL);  /* don't keep track of the thread id */

    if (decode_thread_handle == 0 ||
        SetThreadPriority(decode_thread_handle, AGAVE_API_CONFIG->GetInt(playbackConfigGroupGUID, L"priority", THREAD_PRIORITY_HIGHEST)) == 0)
	{
        close_vgmstream(vgmstream);
        vgmstream = NULL;
        return -1;
	}

    return 0; /* success */
}

/* pause stream */
void pause() {
    paused = 1;
    plugin.outMod->Pause(1);
}

void unpause() {
    paused = 0;
    plugin.outMod->Pause(0);
}

/* return 1 if paused, 0 if not */
int ispaused() {
    return paused;
}

/* stop (unload) stream */
void stop() {
    if (decode_thread_handle != INVALID_HANDLE_VALUE) {
        decode_abort = 1;

        /* arbitrary wait length */
        if (WaitForSingleObject(decode_thread_handle, 1000) == WAIT_TIMEOUT) {
            TerminateThread(decode_thread_handle, 0); // TODO: error?
        }
        CloseHandle(decode_thread_handle);
        decode_thread_handle = INVALID_HANDLE_VALUE;
    }

    close_vgmstream(vgmstream);
    vgmstream = NULL;

    plugin.outMod->Close();
    plugin.SAVSADeInit();
}

/* get length in ms */
int getlength() {
    return stream_length_samples * 1000LL / vgmstream->sample_rate;
}

/* current output time in ms */
int getoutputtime() {
    return decode_pos_ms + (plugin.outMod->GetOutputTime() - plugin.outMod->GetWrittenTime());
}

/* seeks to point in stream (in ms) */
void setoutputtime(int time_in_ms) {
    if (vgmstream)
        seek_needed_samples = (long long)time_in_ms * vgmstream->sample_rate / 1000LL;
}

/* pass these commands through */
void setvolume(int volume) {
	plugin.outMod->SetVolume(volume);
}

void setpan(int pan) {
	plugin.outMod->SetPan(pan);
}

/* display info box (ALT+3) */
int infoDlg(const in_char *fn, HWND hwnd) {
#ifndef UNICODE_INPUT_PLUGIN
    char description[1024] = {0};
    size_t description_size = 1024;

    concatn(description_size, description, PLUGIN_DESCRIPTION "\n\n");

    if (!fn || !*fn) {
        /* no filename = current playing file */
        if (!vgmstream)
            return 0;

        describe_vgmstream(vgmstream, description, description_size);
    }
    else {
        /* some other file in playlist given by filename */
        VGMSTREAM * infostream = NULL;
        in_char filename[PATH_LIMIT] = {0};
        int stream_index = 0;

        /* check for info encoded in the filename */
        parse_fn_string(fn, NULL, filename, PATH_LIMIT);
        parse_fn_int(fn, L"$s", &stream_index);

        infostream = init_vgmstream_winamp(filename, stream_index);
        if (!infostream)
            return 0;

        describe_vgmstream(infostream, description, description_size);

        close_vgmstream(infostream);
        infostream = NULL;
    }

    {
        TCHAR buf[1024] = {0};
        size_t buf_size = 1024;

        cfg_char_to_wchar(buf, buf_size, description);
        MessageBox(hwnd, buf, TEXT("Stream info"), MB_OK);
    }
#endif
    return 0;
}

/* retrieve title (playlist name) and time on the current or other file in the playlist */
void getfileinfo(const in_char *fn, in_char *title, int *length_in_ms) {

    if (!fn || !*fn) {
        /* no filename = current playing file */

        if (!vgmstream)
            return;

        if (title) {
            get_title(title, GETFILEINFO_TITLE_LENGTH, lastfn, vgmstream);
        }

        if (length_in_ms) {
            *length_in_ms = getlength();
        }
    }
    else {
        /* some other file in playlist given by filename */
        VGMSTREAM * infostream = NULL;
        in_char filename[PATH_LIMIT] = {0};
        int stream_index = 0;

        /* check for info encoded in the filename */
        parse_fn_string(fn, NULL, filename,PATH_LIMIT);
        parse_fn_int(fn, L"$s", &stream_index);

        infostream = init_vgmstream_winamp(filename, stream_index);
        if (!infostream) return;

        if (title) {
            get_title(title, GETFILEINFO_TITLE_LENGTH, fn, infostream);
        }

        if (length_in_ms) {
            *length_in_ms = -1000;
            if (infostream) {
                const int num_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, infostream);
                *length_in_ms = num_samples * 1000LL /infostream->sample_rate;
            }
        }

        close_vgmstream(infostream);
        infostream = NULL;
    }
}

/* eq stuff */
void eq_set(int on, char data[10], int preamp) {
}

/* the decode thread */
DWORD WINAPI __stdcall decode(void *arg) {
    const int max_buffer_samples = sizeof(sample_buffer) / sizeof(sample_buffer[0]) / 2 / vgmstream->channels;
    const int max_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, vgmstream);

    while (!decode_abort) {
        int samples_to_do;
        int output_bytes;

        if (decode_pos_samples + max_buffer_samples > stream_length_samples
                && (!loop_forever || !vgmstream->loop_flag))
            samples_to_do = stream_length_samples - decode_pos_samples;
        else
            samples_to_do = max_buffer_samples;

        /* seek setup (max samples to skip if still seeking, mark done) */
        if (seek_needed_samples != -1) {
            /* reset if we need to seek backwards */
            if (seek_needed_samples < decode_pos_samples) {
                reset_vgmstream(vgmstream);

                if (ignore_loop)
                    vgmstream->loop_flag = 0;

                decode_pos_samples = 0;
                decode_pos_ms = 0;
            }

            /* adjust seeking past file, can happen using the right (->) key
             * (should be done here and not in SetOutputTime due to threads/race conditions) */
            if (seek_needed_samples > max_samples) {
                seek_needed_samples = max_samples;
            }

            /* adjust max samples to seek */
            if (decode_pos_samples < seek_needed_samples) {
                samples_to_do = seek_needed_samples - decode_pos_samples;
                if (samples_to_do > max_buffer_samples)
                    samples_to_do = max_buffer_samples;
            }
            else {
                seek_needed_samples = -1;
            }

            /* flush Winamp buffers */
            plugin.outMod->Flush((int)decode_pos_ms);
        }

        output_bytes = (samples_to_do * output_channels * sizeof(short));
        if (plugin.dsp_isactive())
            output_bytes = output_bytes * 2; /* Winamp's DSP may need double samples */

        if (samples_to_do == 0) { /* track finished */
            plugin.outMod->CanWrite();    /* ? */
            if (!plugin.outMod->IsPlaying()) {
                PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0,0); /* end */
                return 0;
            }
            Sleep(10);
        }
        else if (seek_needed_samples != -1) { /* seek */
            render_vgmstream(sample_buffer, samples_to_do, vgmstream);

            /* discard decoded samples and keep seeking */
            decode_pos_samples += samples_to_do;
            decode_pos_ms = decode_pos_samples * 1000LL / vgmstream->sample_rate;
        }
        else if (plugin.outMod->CanWrite() >= output_bytes) { /* decode */
            render_vgmstream(sample_buffer, samples_to_do, vgmstream);

            /* fade near the end */
            if (vgmstream->loop_flag && fade_samples > 0 && !loop_forever) {
                int samples_into_fade = decode_pos_samples - (stream_length_samples - fade_samples);
                if (samples_into_fade + samples_to_do > 0) {
                    int j, k;
                    for (j = 0; j < samples_to_do; j++, samples_into_fade++) {
                        if (samples_into_fade > 0) {
                            const double fadedness = (double)(fade_samples - samples_into_fade) / fade_samples;
                            for (k = 0; k < vgmstream->channels; k++) {
                                sample_buffer[j * vgmstream->channels + k] =
                                    (short)(sample_buffer[j * vgmstream->channels + k] * fadedness);
                            }
                        }
                    }
                }
            }

            /* downmix enabled (useful when the stream's channels are too much for Winamp's output) */
            if (downmix) {
                short temp_buffer[(576*2) * 2] = {0};
                int s, ch;

                /* just copy the first channels for now */
                for (s = 0; s < samples_to_do; s++) {
                    for (ch = 0; ch < output_channels; ch++) {
                        temp_buffer[s*output_channels + ch] = sample_buffer[s*vgmstream->channels + ch];
                    }
                }

                /* copy back to global buffer */
                memcpy(sample_buffer,temp_buffer, samples_to_do*output_channels*sizeof(short));
            }

            /* output samples */
            plugin.SAAddPCMData((char*)sample_buffer, output_channels, 16, decode_pos_ms);
            plugin.VSAAddPCMData((char*)sample_buffer, output_channels, 16, decode_pos_ms);

            if (plugin.dsp_isactive()) { /* find out DSP's needs */
                const int dsp_output_samples = plugin.dsp_dosamples(sample_buffer, samples_to_do, 16,
																	output_channels, vgmstream->sample_rate);
                output_bytes = dsp_output_samples * output_channels * sizeof(short);
            }

            plugin.outMod->Write((char*)sample_buffer, output_bytes);

            decode_pos_samples += samples_to_do;
            decode_pos_ms = decode_pos_samples * 1000LL / vgmstream->sample_rate;
        }
        else { /* can't write right now */
            Sleep(20);
        }
    }

    return 0;
}

/* configuration dialog */
void config(HWND hwndParent) {
    DialogBox(plugin.hDllInstance, MAKEINTRESOURCE(IDD_CONFIG), hwndParent, configDlgProc);
}

/* *********************************** */

/* main plugin def */
In_Module plugin = {
	IN_VER_RET,
    (char*)PLUGIN_DESCRIPTIONW,
    0,  /* hMainWindow (filled in by Winamp) */
    0,  /* hDllInstance (filled in by Winamp) */
    working_extension_list,
    1, /* is_seekable flag  */
    1, /* UsesOutputPlug flag */
    config,
    about,
    init,
    quit,
    getfileinfo,
    infoDlg,
    isourfile,
    play,
    pause,
    unpause,
    ispaused,
    stop,
    getlength,
    getoutputtime,
    setoutputtime,
    setvolume,
    setpan,
    0,0,0,0,0,0,0,0,0, /* vis stuff */
    0,0, /* dsp stuff */
    eq_set,
    NULL, /* SetInfo */
    0 /* outMod */
};

__declspec( dllexport ) In_Module * winampGetInModule2() {
    return &plugin;
}

char *AutoChar(const wchar_t *convert/*, UINT codePage = CP_ACP, UINT flags = 0*/)
{
	const int size = (convert ? WideCharToMultiByte(CP_ACP, 0, convert, (int)-1, 0, 0, NULL, NULL) : 0);
	//const int size = AutoCharSize(convert, (size_t)-1, CP_ACP, flags);

	if (!size)
	{
		return 0;
	}
	else
	{
		char *narrow = (char *)malloc(size);
		if (narrow)
		{
			if (!WideCharToMultiByte(CP_ACP, 0, convert, -1, narrow, size, NULL, NULL))
			{
				free(narrow);
				narrow = 0;
			}
			else
			{
				narrow[size - 1] = 0;
			}
		}
		return narrow;
	}
}

wchar_t *AutoWide(const char *convert)
{
	if (!convert)
	{
		return 0;
	}
	else
	{
		const int size = MultiByteToWideChar(CP_ACP, 0, convert, -1, 0, 0);
		if (!size)
		{
			return 0;
		}
		else
		{
			wchar_t *wide = (wchar_t *)malloc(size << 1);
			if (wide)
			{
				if (!MultiByteToWideChar(CP_ACP, 0, convert, -1, wide,size))
				{
					free(wide);
					wide = 0;
				}
				else
				{
					wide[size - 1] = 0;
				}
			}
			return wide;
		}
	}
}

extern "C" __declspec(dllexport) int winampUninstallPlugin(HINSTANCE hDllInst, HWND hwndDlg, int param)
{
	// TODO
	// prompt to remove our settings with default as no (just incase)
	/*if (MessageBox( hwndDlg, WASABI_API_LNGSTRINGW( IDS_UNINSTALL_SETTINGS_PROMPT ),
				    pluginTitle, MB_YESNO | MB_DEFBUTTON2 ) == IDYES ) {
		WritePrivateProfileString(CONFIG_APP_NAME, 0, 0, get_paths()->plugin_ini_file);
	}*/

	// as we're not hooking anything and have no settings we can support an on-the-fly uninstall action
	return IN_PLUGIN_UNINSTALL_NOW;
}

// return 1 if you want winamp to show it's own file info dialogue, 0 if you want to show your own (via In_Module.InfoBox)
// if returning 1, remember to implement winampGetExtendedFileInfo("formatinformation")!
extern "C" __declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t * fn)
{
	return 1;
}

// should return a child window of 513x271 pixels (341x164 in msvc dlg units), or return NULL for no tab.
// Fill in name (a buffer of namelen characters), this is the title of the tab (defaults to "Advanced").
// filename will be valid for the life of your window. n is the tab number. This function will first be 
// called with n == 0, then n == 1 and so on until you return NULL (so you can add as many tabs as you like).
// The window you return will recieve WM_COMMAND, IDOK/IDCANCEL messages when the user clicks OK or Cancel.
// when the user edits a field which is duplicated in another pane, do a SendMessage(GetParent(hwnd),WM_USER,(WPARAM)L"fieldname",(LPARAM)L"newvalue");
// this will be broadcast to all panes (including yours) as a WM_USER.
extern "C" __declspec(dllexport) HWND winampAddUnifiedFileInfoPane(int n, const wchar_t * filename,
																   HWND parent, wchar_t *name, size_t namelen)
{
	return NULL;
}

extern "C" __declspec (dllexport) int winampGetExtendedFileInfoW(wchar_t *filename, char *metadata, wchar_t *ret, int retlen)
{
	int retval = 0;

	if (!_stricmp(metadata, "type"))
	{
		ret[0] = '0';
		ret[1] = 0;
		return 1;
	}
	else if (!_stricmp(metadata, "family"))
	{
		if (!filename || !filename[0])
		{
			return 0;
		}

		const wchar_t *p = PathFindExtension(filename);
		if (p && *p)
		{
			size_t ext_list_len = 0;
			const char ** ext_list = vgmstream_get_formats(&ext_list_len);

			++p;

			char *extension = AutoChar(p);
			for (size_t i = 0; i < ext_list_len; i++)
			{
				if (!_stricmp(extension, ext_list[i]))
				{
					//StringCchPrintf(ret, retlen, L"%s Audio File", p);
					lstrcpyn(ret, L"Video Game Music File", retlen);
					break;
				}
			}
			return 1;
		}
		return 0;
	}

	if (!filename || !*filename)
	{
		return retval;
	}

	/* even if no file, return a 1 and write "0" */
	if (!_stricmp(metadata, "length"))
	{
		StringCchPrintf(ret, retlen, L"%d", 0);
		retval = 1;
	}

	if ( !_stricmp(metadata, "formatinformation"))
	{
		char *fn = AutoChar(filename);
		if (fn)
		{
			VGMSTREAM * infostream = init_vgmstream(fn);
			if (!infostream)
			{
				free(fn);
				return 0;
			}
			else
			{
				char *description = (char *)calloc(retlen, sizeof(char));
				describe_vgmstream(infostream, description, retlen);
				wcsncpy(ret, AutoWide(description), retlen);
				close_vgmstream(infostream);
				infostream = NULL;
				free(fn);
				free(description);
				retval = 1;
			}
		}
	}
	else if (!_stricmp (metadata, "length"))
	{
		char *fn = AutoChar(filename);
		if (fn)
		{
			VGMSTREAM * infostream = init_vgmstream(fn);
			if (!infostream)
			{
				free(fn);
			}
			else
			{
				StringCchPrintf(ret, retlen, L"%d", get_vgmstream_play_samples(loop_count, fade_seconds,
								fade_delay_seconds, infostream) * 1000LL / infostream->sample_rate);
				close_vgmstream(infostream);
				infostream = NULL;
				retval = 1;
			}
		}
	}
	else if (!_stricmp (metadata, "title"))
	{
		wcsncpy(ret, filename, retlen);
		PathStripPath(ret);
		PathRemoveExtension(ret);
	}

	return retval;
}

/* *********************************** */
/* placeholder for conversion api support */

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t *fn, int *size, int *bps, int *nch, int *srate)
{
	return 0;
}


extern "C" __declspec(dllexport) size_t winampGetExtendedRead_getData(intptr_t handle, char *dest, size_t len, int *killswitch)
{
	return 0;
}

extern "C" __declspec(dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int millisecs)
{
	return 0;
}

extern "C" __declspec(dllexport) void winampGetExtendedRead_close(intptr_t handle)
{
}

/* *********************************** */

FARPROC WINAPI FailHook(unsigned dliNotify, PDelayLoadInfo pdli) {
	if (dliNotify == dliFailLoadLib) {
		HMODULE module = NULL;
		wchar_t *filename = AutoWide(pdli != NULL ? pdli->szDll : ""),
				 filepath[MAX_PATH] = {0};

		if (!plugindir[0]) {
			PathCombine(plugindir, get_paths()->winamp_plugin_dir, L"vgmstream_dlls");
		}

		// we look for the plug-in in the vgmstream_dlls
		// folder and if not there or there is a loading
		// issue then we instead look in the Winamp root
		PathCombine(filepath, plugindir, filename);
		if (PathFileExists(filepath)) {
			module = LoadLibrary(filepath);
			if (module == NULL) {
				GetModuleFileName(NULL, filepath, MAX_PATH);
				PathRemoveFileSpec(filepath);
				PathAppend(filepath, filename);
				module = LoadLibrary(filepath);
			}
		} else {
			GetModuleFileName(NULL, filepath, MAX_PATH);
			PathRemoveFileSpec(filepath);
			PathAppend(filepath, filename);
			module = LoadLibrary(filepath);
		}
		free(filename);
		return (FARPROC)module;
	}
	return 0;
}

PfnDliHook __pfnDliFailureHook2 = FailHook;