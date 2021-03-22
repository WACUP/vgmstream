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
#include <string.h>
#include <ctype.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <delayimp.h>

#include "../src/vgmstream.h"
#include "../src/plugins.h"
#include <sdk/winamp/in2.h>
#include <sdk/winamp/wa_ipc.h>
#include <sdk/winamp/ipc_pe.h>
#include <sdk/nu/AutoWide.h>
#include <sdk/nu/AutoChar.h>
#include <sdk/nu/AutoCharFn.h>

#include <api/service/api_service.h>

#include <api/service/waservicefactory.h>

#include <sdk/Agave/Config/api_config.h>

#include <sdk/Agave/Language/api_language.h>

#include <loader/loader/utils.h>
#include <loader/loader/paths.h>
#include <loader/loader/ini.h>

#include "resource.h"

#ifndef VERSION
#include "../version.h"
#endif

#ifndef VERSIONW
#define VERSIONW L"2.3641"
#endif

#define LIBVGMSTREAM_BUILD "1050-3641-gc9e2016f-wacup"
#define APP_NAME "vgmstream plugin"
#define PLUGIN_DESCRIPTION "vgmstream Decoder v" VERSION
#define PLUGIN_DESCRIPTIONW L"vgmstream Decoder v" VERSIONW
#define INI_NAME "plugin.ini"

/* post when playback stops */
#define WM_WA_MPEG_EOF WM_USER+2

extern In_Module plugin; /* the input module, declared at the bottom of this file */
DWORD WINAPI __stdcall decode(void *arg);

/* Winamp Play extension list, to accept and associate extensions in Windows */
#define EXTENSION_LIST_SIZE   (0x2000 * 8)
#define EXT_BUFFER_SIZE 200
/* fixed list to simplify but could also malloc/free on init/close */
wchar_t working_extension_list[EXTENSION_LIST_SIZE] = {0};

api_language* WASABI_API_LNG = NULL;

wchar_t plugindir[MAX_PATH] = {0};

#define FADE_SECONDS_INI_ENTRY TEXT("fade_seconds")
#define FADE_DELAY_SECONDS_INI_ENTRY TEXT("fade_delay")
#define LOOP_COUNT_INI_ENTRY TEXT("loop_count")
#define LOOP_FOREVER_INI_ENTRY TEXT("loop_forever")
#define IGNORE_LOOP_INI_ENTRY TEXT("ignore_loop")
#define DISABLE_SUBSONGS_INI_ENTRY TEXT("disable_subsongs")
#define DOWNMIX_CHANNELS_INI_ENTRY TEXT("downmix_channels")
#define DISABLE_TAGFILE_INI_ENTRY TEXT("tagfile_disable")
#define FORCE_TITLE_INI_ENTRY TEXT("force_title")
#define EXTS_UNKNOWN_ON TEXT("exts_unknown_on")
#define EXTS_COMMON_ON TEXT("exts_common_on")

double fade_seconds = 10.0;
double fade_delay_seconds = 0.0;
double loop_count = 2.0;
int loop_forever = 0;
int ignore_loop = 0;
int disable_subsongs = 0;
int downmix_channels = 0;
int loaded_config = 0;
int tagfile_disable = 0;
int force_title = 0;
int exts_unknown_on = 0;
int exts_common_on = 0;

/* Winamp needs at least 576 16-bit samples, stereo, doubled in case DSP effects are active */
#define SAMPLE_BUFFER_SIZE 576
short sample_buffer[SAMPLE_BUFFER_SIZE*2 * VGMSTREAM_MAX_CHANNELS]; //todo maybe should be dynamic

/* plugin state */
VGMSTREAM * vgmstream = NULL;
HANDLE decode_thread_handle = INVALID_HANDLE_VALUE;

int paused = 0;
int decode_abort = 0;
int seek_sample = -1;
int decode_pos_ms = 0;
int decode_pos_samples = 0;
int length_samples = 0;
int fade_samples = 0;
int output_channels = 0;
double volume = 1.0;

const wchar_t* tagfile_name = L"!tags.m3u"; //todo make configurable

wchar_t lastfn[FILENAME_SIZE] = { 0 }; /* name of the currently playing file */

/* ************************************* */

/* converts from utf16 to utf8 (if unicode is on) */
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
    FILE *infile_ref; /* pointer to the infile in stdiosf (partially handled by stdiosf) */
} WINAMP_STREAMFILE;

static STREAMFILE *open_winamp_streamfile_by_file(FILE *infile, const char * path);
static STREAMFILE *open_winamp_streamfile_by_wpath(const in_char *wpath);

static size_t wasf_read(WINAMP_STREAMFILE* sf, uint8_t* dest, off_t offset, size_t length) {
    return sf->stdiosf->read(sf->stdiosf, dest, offset, length);
}

static off_t wasf_get_size(WINAMP_STREAMFILE* sf) {
    return sf->stdiosf->get_size(sf->stdiosf);
}

static off_t wasf_get_offset(WINAMP_STREAMFILE* sf) {
    return sf->stdiosf->get_offset(sf->stdiosf);
}

static void wasf_get_name(WINAMP_STREAMFILE* sf, char* buffer, size_t length) {
    sf->stdiosf->get_name(sf->stdiosf, buffer, length);
}

static STREAMFILE *wasf_open(WINAMP_STREAMFILE* sf, const char* const filename, size_t buffersize) {
    in_char wpath[FILENAME_SIZE] = { 0 };

    if (!filename)
        return NULL;

#if !defined (__ANDROID__) && !defined (_MSC_VER)
    /* When enabling this for MSVC it'll seemingly work, but there are issues possibly related to underlying
     * IO buffers when using dup(), noticeable by re-opening the same streamfile with small buffer sizes
     * (reads garbage). This reportedly causes issues in Android too */

    /* if same name, duplicate the file descriptor we already have open */ //unsure if all this is needed
	char name[FILENAME_SIZE] = { 0 };
	sf->stdiosf->get_name(sf->stdiosf, name, FILENAME_SIZE);
    if (sf->infile_ref && !strcmp(name,filename)) {
        int new_fd;
        FILE *new_file;

        if (((new_fd = dup(fileno(sf->infile_ref))) >= 0) && (new_file = wa_fdopen(new_fd))) {
            STREAMFILE *new_sf = open_winamp_streamfile_by_file(new_file, filename);
            if (new_sf)
                return new_sf;
            fclose(new_file);
        }
        if (new_fd >= 0 && !new_file)
            close(new_fd); /* fdopen may fail when opening too many files */

        /* on failure just close and try the default path (which will probably fail a second time) */
    }
#endif

    /* STREAMFILEs carry char/UTF8 names, convert to wchar for Winamp */
	wa_char_to_wchar(wpath, FILENAME_SIZE, filename);
    return open_winamp_streamfile_by_wpath(wpath);
}

static void wasf_close(WINAMP_STREAMFILE* sf) {
    /* closes infile_ref + frees in the internal STDIOSTREAMFILE (fclose for wchar is not needed) */
    sf->stdiosf->close(sf->stdiosf);
    free(sf); /* and the current struct */
}

static STREAMFILE *open_winamp_streamfile_by_file(FILE* file, const char* path) {
    WINAMP_STREAMFILE* this_sf = NULL;
    STREAMFILE* stdiosf = NULL;

    this_sf = (WINAMP_STREAMFILE*)calloc(1,sizeof(WINAMP_STREAMFILE));
    if (!this_sf) goto fail;

    stdiosf = open_stdio_streamfile_by_file(file,path);
    if (!stdiosf) goto fail;

    this_sf->sf.read = (size_t (*)(struct _STREAMFILE *,uint8_t * dest, off_t offset, size_t length))wasf_read;
    this_sf->sf.get_size = (size_t (*)(struct _STREAMFILE *))wasf_get_size;
    this_sf->sf.get_offset = (off_t (*)(struct _STREAMFILE *))wasf_get_offset;
    this_sf->sf.get_name = (void (*)(struct _STREAMFILE *,char *name,size_t length))wasf_get_name;
    this_sf->sf.open = (struct _STREAMFILE * (*)(struct _STREAMFILE *,const char * const filename,size_t buffersize))wasf_open;
    this_sf->sf.close = (void (*)(struct _STREAMFILE *))wasf_close;

    this_sf->stdiosf = stdiosf;
    this_sf->infile_ref = file;

    return &this_sf->sf; /* pointer to STREAMFILE start = rest of the custom data follows */

fail:
    close_streamfile(stdiosf);
    free(this_sf);
    return NULL;
}


static STREAMFILE* open_winamp_streamfile_by_wpath(const in_char* wpath) {
    FILE* infile = NULL;
    STREAMFILE* sf;
    char path[FILENAME_SIZE] = { 0 };

    /* open a FILE from a Winamp (possibly UTF-16) path */
    infile = wa_fopen(wpath);
    if (!infile) {
        /* allow non-existing files in some cases */
        if (!vgmstream_is_virtual_filename(path))
            return NULL;
    }

    /* convert to UTF-8 if needed for internal use */
	wa_wchar_to_char(path, FILENAME_SIZE, wpath);

    sf = open_winamp_streamfile_by_file(infile,path);
    if (!sf) {
        if (infile) fclose(infile);
    }

    return sf;
}

/* opens vgmstream for winamp */
static VGMSTREAM* init_vgmstream_winamp(const in_char* fn, int stream_index) {
    VGMSTREAM* vgmstream = NULL;

    //return init_vgmstream(fn);

    /* manually init streamfile to pass the stream index */
    STREAMFILE* sf = open_winamp_streamfile_by_wpath(fn); //open_stdio_streamfile(fn);
    if (sf) {
        sf->stream_index = stream_index;
        vgmstream = init_vgmstream_from_STREAMFILE(sf);
        close_streamfile(sf);
    }

    return vgmstream;
}


/* ************************************* */
/* IN_CONFIG                             */
/* ************************************* */

/* config */
#define CONFIG_APP_NAME TEXT("vgmstream plugin")
#define CONFIG_INI_NAME TEXT("plugin.ini")

#define DEFAULT_FADE_SECONDS TEXT("10.00")
#define DEFAULT_FADE_DELAY_SECONDS TEXT("0.00")
#define DEFAULT_LOOP_COUNT TEXT("2.00")
#define DEFAULT_LOOP_FOREVER 0
#define DEFAULT_IGNORE_LOOP 0
#define DEFAULT_DISABLE_SUBSONGS 0
#define DEFAULT_DOWNMIX_CHANNELS 0
#define DEFAULT_TAGFILE_DISABLE 0
#define DEFAULT_FORCE_TITLE 0
#define DEFAULT_EXTS_UNKNOWN_ON 0
#define DEFAULT_EXTS_COMMON_ON 0

void read_config(void) {
	if (!loaded_config) {
		wchar_t buf[256] = {0};
		int consumed;

		loaded_config = 1;

		GetNativeIniString(PLUGIN_INI, CONFIG_APP_NAME, FADE_SECONDS_INI_ENTRY, DEFAULT_FADE_SECONDS, buf, ARRAYSIZE(buf));
		if (swscanf(buf, L"%lf%n", &fade_seconds, &consumed) < 1 || consumed != wcslen(buf) || fade_seconds < 0) {
			(void)swscanf(DEFAULT_FADE_SECONDS, L"%lf", &fade_seconds);
		}

		GetNativeIniString(PLUGIN_INI, CONFIG_APP_NAME, FADE_DELAY_SECONDS_INI_ENTRY, DEFAULT_FADE_DELAY_SECONDS, buf, ARRAYSIZE(buf));
		if (swscanf(buf, L"%lf%n", &fade_delay_seconds, &consumed) < 1 || consumed != wcslen(buf)) {
			(void)swscanf(DEFAULT_FADE_DELAY_SECONDS, L"%lf", &fade_delay_seconds);
		}

		GetNativeIniString(PLUGIN_INI, CONFIG_APP_NAME, LOOP_COUNT_INI_ENTRY, DEFAULT_LOOP_COUNT, buf, ARRAYSIZE(buf));
		if (swscanf(buf, L"%lf%n", &loop_count, &consumed) != 1 || consumed != wcslen(buf) || loop_count < 0) {
			(void)swscanf(DEFAULT_LOOP_COUNT, L"%lf", &loop_count);
		}

		loop_forever = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, LOOP_FOREVER_INI_ENTRY, DEFAULT_LOOP_FOREVER);
		ignore_loop = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, IGNORE_LOOP_INI_ENTRY, DEFAULT_IGNORE_LOOP);

		if (loop_forever && ignore_loop) {
			_snwprintf(buf, ARRAYSIZE(buf), L"%d", DEFAULT_LOOP_FOREVER);
			loop_forever = DEFAULT_LOOP_FOREVER;

			_snwprintf(buf, ARRAYSIZE(buf), L"%d", DEFAULT_IGNORE_LOOP);
			ignore_loop = DEFAULT_IGNORE_LOOP;
		}

		disable_subsongs = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, DISABLE_SUBSONGS_INI_ENTRY, DEFAULT_DISABLE_SUBSONGS);

		downmix_channels = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, DOWNMIX_CHANNELS_INI_ENTRY, DEFAULT_DOWNMIX_CHANNELS);
		if (downmix_channels < 0) {
			_snwprintf(buf, ARRAYSIZE(buf), L"%d", DEFAULT_DOWNMIX_CHANNELS);
			SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
								DOWNMIX_CHANNELS_INI_ENTRY, buf);
			downmix_channels = DEFAULT_DOWNMIX_CHANNELS;
		}

		tagfile_disable = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, DISABLE_TAGFILE_INI_ENTRY, DEFAULT_TAGFILE_DISABLE);
		
		force_title = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, FORCE_TITLE_INI_ENTRY, DEFAULT_FORCE_TITLE);

		exts_unknown_on = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, EXTS_UNKNOWN_ON, DEFAULT_EXTS_UNKNOWN_ON);
		exts_common_on = GetNativeIniInt(PLUGIN_INI, CONFIG_APP_NAME, EXTS_COMMON_ON, DEFAULT_EXTS_COMMON_ON);
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

				SetDlgItemInt(hDlg, IDC_DOWNMIX_CHANNELS, downmix_channels, TRUE);

				if (tagfile_disable)
					CheckDlgButton(hDlg, IDC_TAGFILE_DISABLE, BST_CHECKED);

		if (force_title)
			CheckDlgButton(hDlg, IDC_FORCE_TITLE, BST_CHECKED);

				if (exts_unknown_on)
					CheckDlgButton(hDlg, IDC_EXTS_UNKNOWN_ON, BST_CHECKED);

				if (exts_common_on)
					CheckDlgButton(hDlg, IDC_EXTS_COMMON_ON, BST_CHECKED);
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
						int temp_downmix_channels = 0;
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

                        GetDlgItemText(hDlg, IDC_DOWNMIX_CHANNELS, buf, ARRAYSIZE(buf));
                        if (swscanf(buf, L"%d%n", &temp_downmix_channels, &consumed) < 1 ||
							consumed != wcslen(buf) || temp_downmix_channels < 0) {
                            MessageBox(hDlg, L"Invalid value for Downmix Channels\n"
                                    L"Must be a number greater than or equal to zero",
                                    L"Error", MB_OK | MB_ICONERROR);
                            break;
                        }

                        fade_seconds = temp_fade_seconds;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_seconds);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											FADE_SECONDS_INI_ENTRY, buf);

                        fade_delay_seconds = temp_fade_delay_seconds;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", fade_delay_seconds);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											FADE_DELAY_SECONDS_INI_ENTRY, buf);

                        loop_count = temp_loop_count;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%.2lf", loop_count);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											LOOP_COUNT_INI_ENTRY, buf);

                        downmix_channels = temp_downmix_channels;
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", downmix_channels);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											DOWNMIX_CHANNELS_INI_ENTRY, buf);
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
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											LOOP_FOREVER_INI_ENTRY, buf);

                        ignore_loop = (IsDlgButtonChecked(hDlg, IDC_IGNORE_LOOP) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", ignore_loop);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											IGNORE_LOOP_INI_ENTRY, buf);
						break;
					}
				case IDC_DISABLE_SUBSONGS:
					{
						wchar_t buf[256] = {0};
						disable_subsongs = (IsDlgButtonChecked(hDlg, IDC_DISABLE_SUBSONGS) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", disable_subsongs);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											DISABLE_SUBSONGS_INI_ENTRY, buf);
						break;
					}
				case IDC_TAGFILE_DISABLE:
					{
						wchar_t buf[256] = {0};
						tagfile_disable = (IsDlgButtonChecked(hDlg, IDC_TAGFILE_DISABLE) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", tagfile_disable);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											DISABLE_TAGFILE_INI_ENTRY, buf);
						break;
					}
		case IDC_FORCE_TITLE:
		{
			wchar_t buf[256] = { 0 };
			force_title = (IsDlgButtonChecked(hDlg, IDC_FORCE_TITLE) == BST_CHECKED);
			_snwprintf(buf, ARRAYSIZE(buf), L"%d", force_title);
			SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
								FORCE_TITLE_INI_ENTRY, buf);
			break;
		}
				case IDC_EXTS_UNKNOWN_ON:
					{
						wchar_t buf[256] = {0};
						exts_unknown_on = (IsDlgButtonChecked(hDlg, IDC_EXTS_UNKNOWN_ON) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", exts_unknown_on);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											EXTS_UNKNOWN_ON, buf);
						break;
					}
				case IDC_EXTS_COMMON_ON:
					{
						wchar_t buf[256] = {0};
						exts_common_on = (IsDlgButtonChecked(hDlg, IDC_EXTS_COMMON_ON) == BST_CHECKED);
                        _snwprintf(buf, ARRAYSIZE(buf), L"%d", exts_common_on);
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME,
											EXTS_COMMON_ON, buf);
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

						SetDlgItemInt(hDlg, IDC_DOWNMIX_CHANNELS, DEFAULT_DOWNMIX_CHANNELS, TRUE);

						CheckDlgButton(hDlg, IDC_TAGFILE_DISABLE, BST_UNCHECKED);

			CheckDlgButton(hDlg, IDC_FORCE_TITLE, BST_UNCHECKED);

						CheckDlgButton(hDlg, IDC_EXTS_UNKNOWN_ON, BST_UNCHECKED);
						CheckDlgButton(hDlg, IDC_EXTS_COMMON_ON, BST_UNCHECKED);

						// physically remove the section from the ini file as it's quicker
						SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME, 0, 0);
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

    if (disable_subsongs || vgmstream->num_streams <= 1)
        return 0; /* don't split if no subsongs */
    if (stream_index > 0 || vgmstream->stream_index > 0)
        return 0; /* no split if already playing subsong */

    hPlaylistWindow = GetPlaylistWnd()/*(HWND)SendMessage(plugin.hMainWindow, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND)*/;
    playlist_index = GetPlaylistPosition()/*SendMessage(plugin.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTPOS)*/;

    /* The only way to pass info around in Winamp is encoding it into the filename, so a fake name
     * is created with the index. Then, winamp_Play (and related) intercepts and reads the index. */
    for (i = 0; i < vgmstream->num_streams; i++) {
        in_char stream_fn[FILENAME_SIZE] = { 0 };

        make_fn_subsong(stream_fn, FILENAME_SIZE, filename, (i + 1)); /* encode index in filename */

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

    /* autoplay doesn't always advance to the first unpacked track, but manually fails somehow */
    //SendMessage(input_module.hMainWindow,WM_WA_IPC,playlist_index,IPC_SETANDPLAYLISTPOS);

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

#if 0
/* Adds ext to Winamp's extension list */
static void add_extension(char* dst, int dst_len, const char *ext) {
    char buf[EXT_BUFFER_SIZE] = {0};
    char ext_upp[EXT_BUFFER_SIZE] = {0};
    int ext_len, written;
    int i,j;
    if (dst_len <= 1)
        return;

    ext_len = strlen(ext);

    /* find end of dst (double \0), saved in i */
    for (i = 0; i < dst_len - 2 && (dst[i] || dst[i+1]); i++)
        ;

    /* check if end reached or not enough room to add */
    if (i == dst_len - 2 || i + EXT_BUFFER_SIZE+2 > dst_len - 2 || ext_len * 3 + 20+2 > EXT_BUFFER_SIZE) {
        dst[i] = '\0';
        dst[i+1] = '\0';
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
    written = sprintf(buf, "%s%c%s Video Game Music File (*.%s)%c", ext,'\0',ext_upp,ext_upp,'\0');
    for (j = 0; j < written; i++,j++)
        dst[i] = buf[j];
    dst[i] = '\0';
    dst[i+1] = '\0';
}
#endif

/* Creates Winamp's extension list, a single string that ends with \0\0.
 * Each extension must be in this format: "extension\0Description\0" */
static void build_extension_list() {
    size_t ext_list_len = 0, i;
    const char ** ext_list = vgmstream_get_formats(&ext_list_len);

	// this original code provides one entry per extension
	// which is not ideal when the filter list its used in
	// can only go upto 260 characters so instead...
    /*for (i = 0; i < ext_list_len; i++) {
        add_extension(working_extension_list, EXTENSION_LIST_SIZE, ext_list[i]);
    }*/

	// this version keeps it to one filter list entry with
	// all of the extensions reported against it which has
	// the helping factor of making it more likely they'll
	// actually be added into the filter list selection :)
	for (i = 0; i < ext_list_len; i++) {
		if (*working_extension_list)
		{
			wcsncat(working_extension_list, L";", EXTENSION_LIST_SIZE);
		}
		wcsncat(working_extension_list, AutoWide(ext_list[i]), EXTENSION_LIST_SIZE);
	}
	wchar_t *list = working_extension_list + wcslen(working_extension_list) + 1;
	if (list)
	{
		wcscat(list, L"Video Game Music File\0");
	}
	//MessageBox(0, working_extension_list, 0, 0);
}

/* unicode utils */
static void get_title(in_char * dst, int dst_size, const in_char * fn, VGMSTREAM * infostream) {
    in_char *basename;
    in_char buffer[FILENAME_SIZE] = { 0 };
    in_char filename[FILENAME_SIZE] = { 0 };
    int stream_index = 0;

    parse_fn_string(fn, NULL, filename, FILENAME_SIZE);
    parse_fn_int(fn, L"$s", &stream_index);

    basename = (in_char*)filename + wcslen(filename); /* find end */
    while (*basename != '\\' && basename >= filename) /* and find last "\" */
        --basename;
    ++basename;
    wcscpy(dst, basename);

    /* infostream gets added at first with index 0, then once played it re-adds proper numbers */
    if (infostream) {
        const char* info_name = infostream->stream_name;
        int info_streams = infostream->num_streams;
        int info_subsong = infostream->stream_index;
        int is_first = infostream->stream_index == 0;

        /* show number if file has more than 1 subsong */
        if (info_streams > 1) {
            if (is_first)
                _snwprintf(buffer, FILENAME_SIZE, L"#1~%i", info_streams);
            else
                _snwprintf(buffer, FILENAME_SIZE, L"#%i", info_subsong);
        wcscat(dst, buffer);
    }

		/* show name if file has subsongs (implicitly shows also for TXTP) */
		if (info_name[0] != '\0' && ((info_streams > 0 && !is_first) || info_streams == 1 || force_title)) {
			_snwprintf(buffer, FILENAME_SIZE, L" (%hs)", info_name);
			wcscat(dst,buffer);
		}
	}

    /* show name, but not for the base stream */
    if (infostream && infostream->stream_name[0] != '\0' && stream_index > 0) {
        in_char stream_name[FILENAME_SIZE] = { 0 };
        wa_char_to_wchar(stream_name, FILENAME_SIZE, infostream->stream_name);
        _snwprintf(buffer, FILENAME_SIZE, L" (%s)", stream_name);
        wcscat(dst, buffer);
    }
}

static int winampGetExtendedFileInfo_common(in_char* filename, char *metadata, char* ret, int retlen);

static double get_album_gain_volume(const in_char *fn) {
	// this needs to be gone through & compared to
	// what is wanted as this doesn't match up to
	// the api_config that the other plug-ins use
#if 0
    char replaygain[64];
    double gain = 0.0;
    int had_replaygain = 0;
    const int gain_type = plugin.config->GetUnsigned(playbackConfigGroupGUID, L"replaygain_source", 0);

    replaygain[0] = '\0'; /* reset each time to make sure we read actual tags */
    if (gain_type == 1	// album
            && winampGetExtendedFileInfo_common((in_char *)fn, "replaygain_album_gain", replaygain, sizeof(replaygain))
            && replaygain[0] != '\0') {
        gain = atof(replaygain);
        had_replaygain = 1;
    }

    replaygain[0] = '\0';
    if (!had_replaygain
            && winampGetExtendedFileInfo_common((in_char *)fn, "replaygain_track_gain", replaygain, sizeof(replaygain))
            && replaygain[0] != '\0') {
        gain = atof(replaygain);
        had_replaygain = 1;
    }

    if (had_replaygain) {
        double vol = pow(10.0, gain / 20.0);
        double peak = 1.0;

        replaygain[0] = '\0';
        const int clip_type = plugin.config->GetUnsigned(playbackConfigGroupGUID, L"replaygain_mode", 1);
        if (settings.clip_type == 1	// album
                && winampGetExtendedFileInfo_common((in_char *)fn, "replaygain_album_peak", replaygain, sizeof(replaygain))
                && replaygain[0] != '\0') {
            peak = atof(replaygain);
        }
        else if (settings.clip_type == 0	// track
                && winampGetExtendedFileInfo_common((in_char *)fn, "replaygain_track_peak", replaygain, sizeof(replaygain))
                && replaygain[0] != '\0') {
            peak = atof(replaygain);
        }
        return peak != 1.0 ? min(vol, 1.0 / peak) : vol;
    }
#endif
    return 1.0;
}


/* ***************************************** */
/* IN_VGMSTREAM                              */
/* ***************************************** */

void about(HWND hwndParent)
{
    // TODO need to ensure that we keep the build # correct
    AboutMessageBox(hwndParent, PLUGIN_DESCRIPTIONW L"\n\n"
                    L"Copyright © 2008-2021 hcs, FastElbja, manakoAT, bxaimc,\n"
                    L"snakemeat, soneek, kode54, bnnm and all other contributors.\n\n"
                    L"WACUP integration updates by Darren Owen aka DrO\n\n"
                    L"Build date: " TEXT(__DATE__) L"\n\n"
                    L"Using libvgmstream build: " TEXT(LIBVGMSTREAM_BUILD) L"\n\n\n"
                    L"This adds support for dozens of streamed ADPCM and other\n"
                    L"formats extracted from various console and PC video games.\n\n"
                    L"See https://hcs64.com/vgmstream.html for more information\n"
                    L"about the project, source code and alternative versions, etc.",
                    L"vgmstream Decoder");
}

/* loading optimisation to reduce initial blocking 
** andsave building the list if there's no need...
*/
void __cdecl GetFileExtensions(void)
{
    static bool loaded_extensions;
    if (!loaded_extensions)
    {
        /* dynamically make a list of supported extensions */
        build_extension_list();
        plugin.FileExtensions = (char *)working_extension_list;
        loaded_extensions = true;
    }
}

/* called at program init */
int init(void) {
	WASABI_API_LNG = plugin.language;

    // need to have this initialised before we try to do anything with localisation features
    /*WASABI_API_START_LANG(plugin.hDllInstance, InWvLangGuid);

    StringCchPrintf(pluginTitle, ARRAYSIZE(pluginTitle), WASABI_API_LNGSTRINGW(IDS_PLUGIN_NAME), PLUGIN_VERSION);
    plugin.description = (char*)pluginTitle;*/

    // TODO localise
    plugin.description = (char*)PLUGIN_DESCRIPTIONW;

    return IN_INIT_SUCCESS;
}

/* called at program quit */
void quit(void) {
}

/* called before extension checks, to allow detection of mms://, etc */
// TODO need to get this just working nicely with WACUP to avoid conflict
int isourfile(const in_char *fn) {
    if (fn && *fn && !PathIsURL(fn))
    {
        //const in_char *filename;
        const in_char *extension;

        /* get basename + extension */
        //filename = fn;
#if 0
    //must detect empty extensions in folders with . in the name; doesn't work ok?
    filename = wcsrrchr(fn, L'\\');
    if (filename == NULL)
        filename = fn;
    else
        filename++;
#endif
        extension = wcsrchr(fn, L'.');
    if (extension == NULL)
            //return 1; /* extensionless, try to play it */
            return 0; /* extensionless, avoid playing it */
    else
        extension++;

	vgmstream_ctx_valid_cfg cfg = {0};
    cfg.skip_standard = 1; /* validated by Winamp */
    cfg.accept_unknown = exts_unknown_on;
    cfg.accept_common = exts_common_on;

    /* Winamp seem to have bizarre handling of MP3 without standard names (ex song.mp3a),
     * in that it'll try to open normally, rejected if unknown_exts_on is not set, and
     * finally retry with "hi.mp3", accepted if exts_common_on is set. */

    /* returning 0 here means it only accepts the extensions in working_extension_list */
        char filename_utf8[FILENAME_SIZE];
        wa_wchar_to_char(filename_utf8, FILENAME_SIZE, fn);
    return vgmstream_ctx_is_valid(filename_utf8, &cfg);
    }
    return 0;
}

/* request to start playing a file */
int play(const in_char *fn) {
    int max_latency;
    in_char filename[FILENAME_SIZE] = { 0 };
    int stream_index = 0;

    read_config();

    if (vgmstream)
        return 1;

    /* check for info encoded in the filename */
    parse_fn_string(fn, NULL, filename, FILENAME_SIZE);
    parse_fn_int(filename, L"$s", &stream_index);

    /* open the stream */
    vgmstream = init_vgmstream_winamp(filename,stream_index);
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
    if (downmix_channels > 0 && downmix_channels < vgmstream->channels)
        output_channels = downmix_channels;

    /* enable after all config but before outbuf (though ATM outbuf is not dynamic so no need to read input_channels) */
//    vgmstream_mixing_autodownmix(vgmstream, downmix_channels);
//    vgmstream_mixing_enable(vgmstream, SAMPLE_BUFFER_SIZE, NULL /*&input_channels*/, &output_channels);

    /* save original name */
    wcsncpy(lastfn, filename, FILENAME_SIZE);

    /* open the output plugin */
    max_latency = (plugin.outMod ? plugin.outMod->Open(vgmstream->sample_rate, output_channels, 16, -1, -1) : -1);

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

    plugin.outMod->SetVolume(-666);

    /* reset internals */
	paused = 0;
    decode_abort = 0;
    seek_sample = -1;
    decode_pos_ms = 0;
    decode_pos_samples = 0;
    length_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, vgmstream);
    fade_samples = (int)(fade_seconds * vgmstream->sample_rate);
	volume = get_album_gain_volume(filename);

    /* start */
    decode_thread_handle = CreateThread(NULL,   /* handle cannot be inherited */
										0,      /* stack size, 0=default */
										decode, /* thread start routine */
										NULL,   /* no parameter to start routine */
		CREATE_SUSPENDED, /* wait to set the priority */
										NULL);  /* don't keep track of the thread id */

    if (decode_thread_handle == 0 ||
        SetThreadPriority(decode_thread_handle, plugin.config->GetInt(playbackConfigGroupGUID, L"priority", THREAD_PRIORITY_HIGHEST)) == 0)
    {
        close_vgmstream(vgmstream);
        vgmstream = NULL;
        return -1;
    }
    ResumeThread(decode_thread_handle);
    return 0; /* success */
}

/* pause stream */
void pause(void) {
    paused = 1;
    plugin.outMod->Pause(1);
}

void unpause(void) {
    paused = 0;
    plugin.outMod->Pause(0);
}

/* return 1 if paused, 0 if not */
int ispaused(void) {
    return paused;
}

/* stop (unload) stream */
void stop(void) {
    if (decode_thread_handle != INVALID_HANDLE_VALUE) {
        decode_abort = 1;

        /* arbitrary wait milliseconds (error can trigger if the system is *really* busy) */
        if (WaitForSingleObject(decode_thread_handle, 5000) == WAIT_TIMEOUT) {
            MessageBox(plugin.hMainWindow, TEXT("Error stopping decode thread\n"), TEXT("Error"),MB_OK|MB_ICONERROR);
            TerminateThread(decode_thread_handle, 0);
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
int getlength(void) {
    return (vgmstream ? length_samples * 1000LL / vgmstream->sample_rate : 0);
}

/* current output time in ms */
int getoutputtime(void) {
	if (vgmstream) {
		int32_t pos_ms = decode_pos_ms;

		/* pretend we have reached destination if called while seeking is on */
		if (seek_sample >= 0)
			pos_ms = seek_sample * 1000LL / vgmstream->sample_rate;

		return pos_ms + (plugin.outMod->GetOutputTime() - plugin.outMod->GetWrittenTime());
	}
	return 0;
}

/* seeks to point in stream (in ms) */
void setoutputtime(int time_in_ms) {
    if (vgmstream)
        seek_sample = (long long)time_in_ms * vgmstream->sample_rate / 1000LL;
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
        {
            return INFOBOX_UNCHANGED;
        }

        describe_vgmstream(vgmstream, description, description_size);
    }
    else {
        /* some other file in playlist given by filename */
        VGMSTREAM * infostream = NULL;
        in_char filename[FILENAME_SIZE] = { 0 };
        int stream_index = 0;

        /* check for info encoded in the filename */
        parse_fn_string(fn, NULL, filename, FILENAME_SIZE);
        parse_fn_int(fn, L"$s", &stream_index);

        infostream = init_vgmstream_winamp(filename, stream_index);
        if (!infostream)
        {
            return INFOBOX_UNCHANGED;
        }

        describe_vgmstream(infostream,description,description_size);

        close_vgmstream(infostream);
        infostream = NULL;
    }

    {
        MessageBox(hwnd, AutoWide(description, CP_UTF8), TEXT("Stream info"), MB_OK);
    }
#endif
    return INFOBOX_UNCHANGED;
}

/* retrieve title (playlist name) and time on the current or other file in the playlist */
void getfileinfo(const in_char *fn, in_char *title, int *length_in_ms){

    if (!fn || !*fn) {
        /* no filename = current playing file */

        if (!vgmstream)
            return;

        if (title) {
            get_title(title,GETFILEINFO_TITLE_LENGTH, lastfn, vgmstream);
        }

        if (length_in_ms) {
            *length_in_ms = getlength();
        }
    }
    else {
        /* some other file in playlist given by filename */
        VGMSTREAM * infostream = NULL;
        in_char filename[FILENAME_SIZE] = { 0 };
        int stream_index = 0;

        /* check for info encoded in the filename */
        parse_fn_string(fn, NULL, filename, FILENAME_SIZE);
        parse_fn_int(fn, L"$s", &stream_index);

        infostream = init_vgmstream_winamp(filename, stream_index);
        if (!infostream) return;


        vgmstream_mixing_autodownmix(infostream, downmix_channels);
        vgmstream_mixing_enable(infostream, 0, NULL, NULL);

        if (title) {
            get_title(title,GETFILEINFO_TITLE_LENGTH, fn, infostream);
        }

        if (length_in_ms) {
            *length_in_ms = -1000;
            if (infostream) {
                const int num_samples = vgmstream_get_samples(infostream);
                *length_in_ms = num_samples * 1000LL /infostream->sample_rate;
            }
        }

        close_vgmstream(infostream);
        infostream = NULL;
    }
}

static void do_seek(VGMSTREAM* vgmstream) {
    int play_forever = vgmstream_get_play_forever(vgmstream);
    int this_seek_sample = seek_sample;  /* local due to threads/race conditions changing state->seek_sample elsewhere */

    /* ignore seeking past file, can happen using the right (->) key, ok if playing forever */
    if (this_seek_sample > length_samples && !play_forever) {
		this_seek_sample = -1;
        //state->seek_sample = state->length_samples;
        //seek_sample = state->length_samples;

        decode_pos_samples = length_samples;
        decode_pos_ms = decode_pos_samples * 1000LL / vgmstream->sample_rate;
        return;
    }

    /* could divide in N seeks (from pos) for slower files so cursor moves, but doesn't seem too necessary */
    seek_vgmstream(vgmstream, this_seek_sample);

    decode_pos_samples = this_seek_sample;
    decode_pos_ms = decode_pos_samples * 1000LL / vgmstream->sample_rate;

    /* different sample: other seek may have been requested during seek_vgmstream */
    if (this_seek_sample == seek_sample)
        seek_sample = -1;
}

/* the decode thread */
DWORD WINAPI __stdcall decode(void *arg) {
    const int max_buffer_samples = sizeof(sample_buffer) / sizeof(sample_buffer[0]) / 2 / vgmstream->channels;
    const int max_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, vgmstream);

    while (!decode_abort) {
        int samples_to_do;
        int output_bytes;

        if (decode_pos_samples + max_buffer_samples > length_samples
                && (!loop_forever || !vgmstream->loop_flag))
            samples_to_do = length_samples - decode_pos_samples;
            if (samples_to_do < 0) /* just in case */
                samples_to_do = 0;
        else {
            samples_to_do = max_buffer_samples;
        }

        output_bytes = (samples_to_do * output_channels * sizeof(short));
        if (plugin.dsp_isactive())
            output_bytes = output_bytes * 2; /* Winamp's DSP may need double samples */

        if (samples_to_do == 0 && seek_sample < 0) { /* track finished and not seeking */
            plugin.outMod->CanWrite();    /* ? */
            if (!plugin.outMod->IsPlaying()) {
                PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0,0); /* end */
                if (decode_thread_handle)
                {
                    CloseHandle(decode_thread_handle);
                    decode_thread_handle = 0;
                }
                return 0;
            }
            Sleep(10);
        }
        else if (seek_sample >= 0) { /* seek */
            do_seek(vgmstream);

            /* flush Winamp buffers *after* fully seeking (allows to play
               buffered samples while we seek, feels a bit snappier) */
            if (seek_sample < 0)
                plugin.outMod->Flush(decode_pos_ms);
        }
        // TODO double-check this but the change to output_bytes
        //		get notsoyasapi working which didn't behave like
        //		all other tested output plug-ins that I've tried
        else if (plugin.outMod->CanWrite() >= samples_to_do/*output_bytes*/) { /* decode */
            render_vgmstream(sample_buffer, samples_to_do, vgmstream);

            /* fade near the end */
            if (vgmstream->loop_flag && fade_samples > 0 && !loop_forever) {
                int samples_into_fade = decode_pos_samples - (length_samples - fade_samples);
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

    if (decode_thread_handle)
    {
        CloseHandle(decode_thread_handle);
        decode_thread_handle = 0;
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
	IN_VER_WACUP,
    (char*)L"wacup(in_vgmstream.dll)",
    0,  /* hMainWindow (filled in by Winamp) */
    0,  /* hDllInstance (filled in by Winamp) */
    NULL,
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
    NULL,
    NULL, /* SetInfo */
    0, /* outMod */
    0, /* api_service */
    GetFileExtensions, /* loading optimisation :) */
	IN_INIT_WACUP_END_STRUCT
};

extern "C" __declspec(dllexport) In_Module * winampGetInModule2(void) {
    return &plugin;
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

/* ************************************* */
/* IN_TAGS                               */
/* ************************************* */

/* could malloc and stuff but totals aren't much bigger than PATH_LIMITs anyway */
#define WINAMP_TAGS_ENTRY_MAX      30
#define WINAMP_TAGS_ENTRY_SIZE     2048

typedef struct {
    int loaded;
    in_char filename[FILENAME_SIZE]; /* tags are loaded for this file */
    int tag_count;

    char keys[WINAMP_TAGS_ENTRY_MAX][WINAMP_TAGS_ENTRY_SIZE+1];
    char vals[WINAMP_TAGS_ENTRY_MAX][WINAMP_TAGS_ENTRY_SIZE+1];
} winamp_tags;

winamp_tags last_tags;


/* Loads all tags for a filename in a temp struct to improve performance, as
 * Winamp requests one tag at a time and may reask for the same tag several times */
static void load_tagfile_info(in_char *filename) {
    STREAMFILE *tagFile = NULL;
    in_char filename_clean[FILENAME_SIZE];
    char filename_utf8[FILENAME_SIZE];
    char tagfile_path_utf8[FILENAME_SIZE];
    in_char tagfile_path_i[FILENAME_SIZE];
    char *path;


    if (tagfile_disable) { /* reset values if setting changes during play */
        last_tags.loaded = 0;
        last_tags.tag_count = 0;
        return;
    }

    /* clean extra part for subsong tags */
    parse_fn_string(filename, NULL, filename_clean, FILENAME_SIZE);

    if (wcscmp(last_tags.filename, filename_clean) == 0) {
        return; /* not changed, tags still apply */
    }

    last_tags.loaded = 0;

    /* tags are now for this filename, find tagfile path */
    wa_wchar_to_char(filename_utf8, FILENAME_SIZE, filename_clean);
    strcpy(tagfile_path_utf8,filename_utf8);

    path = strrchr(tagfile_path_utf8,'\\');
    if (path != NULL) {
        path[1] = '\0'; /* includes "\", remove after that from tagfile_path */
        strcat(tagfile_path_utf8,AutoChar(tagfile_name));
    }
    else { /* ??? */
        strncpy(tagfile_path_utf8,AutoChar(tagfile_name),ARRAYSIZE(tagfile_path_utf8));
    }
    wa_char_to_wchar(tagfile_path_i, FILENAME_SIZE, tagfile_path_utf8);

    wcsncpy(last_tags.filename, filename_clean, ARRAYSIZE(last_tags.filename));
    last_tags.tag_count = 0;

    /* load all tags from tagfile */
    tagFile = open_winamp_streamfile_by_wpath(tagfile_path_i);
    if (tagFile != NULL) {
        const char *tag_key, *tag_val;
        int i;

        VGMSTREAM_TAGS *tags = vgmstream_tags_init(&tag_key, &tag_val);
        vgmstream_tags_reset(tags, filename_utf8);
        while (vgmstream_tags_next_tag(tags, tagFile)) {
            int repeated_tag = 0;
            int current_tag = last_tags.tag_count;
            if (current_tag >= WINAMP_TAGS_ENTRY_MAX)
                continue;

            /* should overwrite repeated tags as global tags may appear multiple times */
            for (i = 0; i < current_tag; i++) {
                if (strcmp(last_tags.keys[i], tag_key) == 0) {
                    current_tag = i;
                    repeated_tag = 1;
                    break;
                }
            }

            last_tags.keys[current_tag][0] = '\0';
            strncat(last_tags.keys[current_tag], tag_key, WINAMP_TAGS_ENTRY_SIZE);
            last_tags.vals[current_tag][0] = '\0';
            strncat(last_tags.vals[current_tag], tag_val, WINAMP_TAGS_ENTRY_SIZE);
            if (!repeated_tag)
                last_tags.tag_count++;
        }

        vgmstream_tags_close(tags);
        close_streamfile(tagFile);
        last_tags.loaded = 1;
    }
}

/* Winamp repeatedly calls this for every known tag currently used in the Advanced Title Formatting (ATF)
 * config, 'metadata' being the requested tag. Returns 0 on failure/tag not found.
 * May be called again after certain actions (adding file to playlist, Play, GetFileInfo, etc), and
 * doesn't seem the plugin can tell Winamp all tags it supports at once or use custom tags. */
//todo unicode stuff could be improved... probably
static int winampGetExtendedFileInfo_common(in_char *filename, char *metadata, char* ret, int retlen) {
    int i, tag_found;
    int max_len;

    /* load list current tags, if necessary */
    load_tagfile_info(filename);
    if (!last_tags.loaded) { /* tagfile not found, fail so default get_title takes over */
        // this needs to be returned where possible
        // otherwise a slew of things will fail to
        // work as expected (e.g. waveform seeker)
        if (strcasecmp(metadata, "length") == 0) {
            goto get_length;
        }
        goto fail;
    }

    /* always called (value in ms), must return ok so other tags get called */
    if (strcasecmp(metadata, "length") == 0) {
get_length:
        //strcpy(ret, "0");//todo should export but shows GetFileInfo's ms if not provided
        char *fn = AutoCharDup(filename);
        if (fn)
        {
            VGMSTREAM * infostream = init_vgmstream(fn);
            if (!infostream)
            {
                free(fn);
            }
            else
            {
                _itoa_s(get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, infostream) *
                        1000LL / infostream->sample_rate, ret, retlen, 10);
                close_vgmstream(infostream);
                infostream = NULL;
            }
        }
        return 1;
    }

    /* find requested tag */
    tag_found = 0;
    max_len = (retlen > 0) ? retlen-1 : retlen;
    for (i = 0; i < last_tags.tag_count; i++) {
        if (strcasecmp(metadata,last_tags.keys[i]) == 0) {
            ret[0] = '\0';
            strncat(ret, last_tags.vals[i], max_len);
            tag_found = 1;
            break;
        }
    }

    /* if tagfile exists but TITLE doesn't Winamp won't default to GetFileInfo, so call it
     * manually as it's useful for files with stream names */
    if (!tag_found && strcasecmp(metadata, "title") == 0) {
        in_char ret_wchar[2048] = {0};

        getfileinfo(filename, ret_wchar, NULL);
        wa_wchar_to_char(ret, retlen, ret_wchar);
        return 1;
    }

    if (!tag_found)
        goto fail;

    return 1;

fail:
    //TODO: is this always needed for Winamp to use replaygain?
    //strcpy(ret, "1.0"); //should set some default value?
    return strcasecmp(metadata, "replaygain_track_gain") == 0 ? 1 : 0;
}

extern "C" __declspec(dllexport) int winampGetExtendedFileInfoW(wchar_t *filename, char *metadata, wchar_t *ret, int retlen)
{
	int retval = 0;

	if (!_stricmp(metadata, "type") ||
		!_stricmp(metadata, "streammetadata"))
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

			char *extension = AutoCharDup(p);
			for (size_t i = 0; i < ext_list_len; i++)
			{
				if (!_stricmp(extension, ext_list[i]))
				{
					//StringCchPrintf(ret, retlen, L"%s Audio File", p);
					lstrcpyn(ret, L"Video Game Music File", retlen);
					break;
				}
			}
			free(extension);
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
		lstrcpyn(ret, L"0", retlen);
		retval = 1;
	}

	if (!_stricmp(metadata, "formatinformation"))
	{
		char *fn = AutoCharDup(filename);
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

	// depending on what the user has set we want to at
	// least provide something so the playlist is nicer
	if (tagfile_disable)
	{
		if (!_stricmp(metadata, "length"))
		{
			char *fn = AutoCharDup(filename);
			if (fn)
			{
				VGMSTREAM * infostream = init_vgmstream(fn);
				if (!infostream)
				{
					free(fn);
				}
				else
				{
					_itow_s(get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, infostream) *
													   1000LL / infostream->sample_rate, ret, retlen, 10);
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
	}
	else
	{
		char ret_utf8[2048] = {0};
		retval = winampGetExtendedFileInfo_common(filename, metadata, ret_utf8, ARRAYSIZE(ret_utf8));
		if (retval)
		{
			wa_char_to_wchar(ret, retlen, ret_utf8);
		}
	}

	return retval;
}

/* *********************************** */
/* placeholder for conversion api support */

// TODO need to finish this off so it's not using globals

short ext_sample_buffer[SAMPLE_BUFFER_SIZE * 2 * VGMSTREAM_MAX_CHANNELS]; //todo maybe should be dynamic

int ext_seek_needed_samples = -1;
int ext_decode_pos_samples = 0;
int ext_stream_length_samples = -1;
int ext_fade_samples = 0;
int ext_output_channels = 0;
double ext_volume = 1.0;

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t *fn, int *size, int *bps, int *nch, int *srate)
{
	read_config();

	VGMSTREAM *ext_vgmstream = NULL;
	in_char filename[FILENAME_SIZE] = { 0 };
	int stream_index = 0;

	/* check for info encoded in the filename */
	parse_fn_string(fn, NULL, filename, FILENAME_SIZE);
	parse_fn_int(fn, L"$s", &stream_index);

	/* open the stream */
	ext_vgmstream = init_vgmstream_winamp(filename, stream_index);
	if (!ext_vgmstream) {
		return NULL;
	}

	/* config */
	/*set_config_defaults(&ext_config);
	apply_config(ext_vgmstream, &ext_config);*/

	/* enable after all config but before outbuf (though ATM outbuf is not dynamic so no need to read input_channels) */
	vgmstream_mixing_autodownmix(ext_vgmstream, downmix_channels);
	vgmstream_mixing_enable(ext_vgmstream, SAMPLE_BUFFER_SIZE, NULL /*&input_channels*/, &ext_output_channels);

	/* reset internals */
	ext_seek_needed_samples = -1;
	ext_decode_pos_samples = 0;
	ext_stream_length_samples = get_vgmstream_play_samples(loop_count, fade_seconds, fade_delay_seconds, ext_vgmstream);
	ext_fade_samples = (int)(fade_seconds * ext_vgmstream->sample_rate);
	ext_volume = 1.0; /* unused */

	if (size) {
		*size = ext_stream_length_samples * ext_output_channels * 2;
	}

	if (bps) {
		*bps = 16;
	}

	if (nch) {
		*nch = ext_output_channels;
	}

	if (srate) {
		*srate = ext_vgmstream->sample_rate;
	}

	return (intptr_t)ext_vgmstream;
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_getData(intptr_t handle, char *dest, size_t len, int *killswitch)
{
	const int max_buffer_samples = SAMPLE_BUFFER_SIZE;
	const int max_samples = ext_stream_length_samples;
	unsigned copied = 0;
	int done = 0;

	VGMSTREAM *ext_vgmstream = (VGMSTREAM *)handle;
	if (!ext_vgmstream) {
		return 0;
	}

	while (copied + max_buffer_samples * ext_vgmstream->channels * 2 < len && !done) {
		int samples_to_do;
		if (ext_decode_pos_samples + max_buffer_samples > ext_stream_length_samples
			&& (!loop_forever || !ext_vgmstream->loop_flag))
			samples_to_do = ext_stream_length_samples - ext_decode_pos_samples;
		else
			samples_to_do = max_buffer_samples;

		/* seek setup (max samples to skip if still seeking, mark done) */
		if (ext_seek_needed_samples != -1) {
			/* reset if we need to seek backwards */
			if (ext_seek_needed_samples < ext_decode_pos_samples) {
				reset_vgmstream(ext_vgmstream);
				//apply_config(ext_vgmstream, &ext_config); /* config is undone by reset */

				ext_decode_pos_samples = 0;
			}

			/* adjust seeking past file, can happen using the right (->) key
			 * (should be done here and not in SetOutputTime due to threads/race conditions) */
			if (ext_seek_needed_samples > max_samples && !loop_forever) {
				ext_seek_needed_samples = max_samples;
			}

			/* adjust max samples to seek */
			if (ext_decode_pos_samples < ext_seek_needed_samples) {
				samples_to_do = ext_seek_needed_samples - ext_decode_pos_samples;
				if (samples_to_do > max_buffer_samples) {
					samples_to_do = max_buffer_samples;
				}
			}
			else {
				ext_seek_needed_samples = -1;
			}
		}

		if (!samples_to_do) { /* track finished */
			break;
		}
		else if (ext_seek_needed_samples != -1) { /* seek */
			render_vgmstream(ext_sample_buffer, samples_to_do, ext_vgmstream);

			/* discard decoded samples and keep seeking */
			ext_decode_pos_samples += samples_to_do;
		}
		else { /* decode */
			render_vgmstream(ext_sample_buffer, samples_to_do, ext_vgmstream);

			/* fade near the end */
			if (ext_vgmstream->loop_flag && ext_fade_samples > 0 && !loop_forever) {
				int fade_channels = ext_output_channels;
				int samples_into_fade = ext_decode_pos_samples - (ext_stream_length_samples - ext_fade_samples);
				if (samples_into_fade + ext_decode_pos_samples > 0) {
					int j, k;
					for (j = 0; j < samples_to_do; j++, samples_into_fade++) {
						if (samples_into_fade > 0) {
							const double fadedness = (double)(ext_fade_samples - samples_into_fade) / ext_fade_samples;
							for (k = 0; k < fade_channels; k++) {
								ext_sample_buffer[j*fade_channels + k] =
									(short)(ext_sample_buffer[j*fade_channels + k] * fadedness);
							}
						}
					}
				}
			}

			/* output samples */
			memcpy(&dest[copied], ext_sample_buffer, samples_to_do * ext_output_channels * 2);
			copied += samples_to_do * ext_output_channels * 2;

			ext_decode_pos_samples += samples_to_do;
		}

		/* check decoding cancelled */
		if (killswitch && *killswitch) {
			break;
		}
	}

	return copied;
}

extern "C" __declspec(dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int millisecs)
{
	VGMSTREAM *ext_vgmstream = (VGMSTREAM *)handle;
	if (ext_vgmstream) {
		ext_seek_needed_samples = (long long)millisecs * ext_vgmstream->sample_rate / 1000LL;
		return 1;
	}
	return 0;
}

extern "C" __declspec(dllexport) void winampGetExtendedRead_close(intptr_t handle)
{
	VGMSTREAM *ext_vgmstream = (VGMSTREAM *)handle;
	if (ext_vgmstream) {
		close_vgmstream(ext_vgmstream);
	}
}

extern "C" __declspec(dllexport) int winampUninstallPlugin(HINSTANCE hDllInst, HWND hwndDlg, int param)
{
	// TODO
	// prompt to remove our settings with default as no (just incase)
	/*if (MessageBox( hwndDlg, WASABI_API_LNGSTRINGW( IDS_UNINSTALL_SETTINGS_PROMPT ),
				    pluginTitle, MB_YESNO | MB_DEFBUTTON2 ) == IDYES ) {
		SaveNativeIniString(PLUGIN_INI, CONFIG_APP_NAME, 0, 0);
	}*/

	// as we're not hooking anything and have no settings we can support an on-the-fly uninstall action
	return IN_PLUGIN_UNINSTALL_NOW;
}


/* *********************************** */

FARPROC WINAPI FailHook(unsigned dliNotify, PDelayLoadInfo pdli) {
	if (dliNotify == dliFailLoadLib) {
		HMODULE module = NULL;
		wchar_t *filename = AutoWideDup(pdli != NULL ? pdli->szDll : ""),
				 filepath[MAX_PATH] = {0};

		if (!plugindir[0]) {
			PathCombine(plugindir, GetPaths()->winamp_plugin_dir, L"vgmstream_dlls\\");
		}

		// we look for the plug-in in the vgmstream_dlls
		// folder and if not there or there is a loading
		// issue then we instead look in the Winamp root
		PathCombine(filepath, plugindir, filename);

		if (PathFileExists(filepath)) {
			// because the ffmpeg dlls have a dependency
			// on themselves, this change maintains the
			// safe search loading whilst just for this
			// attempt it resolves with our custom path
			module = LoadLibraryEx(filepath, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
			if (module == NULL) {
				GetModuleFileName(NULL, filepath, MAX_PATH);
				PathRemoveFileSpec(filepath);
				PathAppend(filepath, filename);
				module = LoadLibrary(filepath);
			}
		}
		else {
			GetModuleFileName(NULL, filepath, MAX_PATH);
			PathRemoveFileSpec(filepath);
			PathAppend(filepath, filename);
			// because the ffmpeg dlls have a dependency
			// on themselves, this change maintains the
			// safe search loading whilst just for this
			// attempt it resolves with our custom path
			module = LoadLibraryEx(filepath, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
		}
		free(filename);
		return (FARPROC)module;
	}
	return 0;
}

ExternC const PfnDliHook __pfnDliFailureHook2 = FailHook;