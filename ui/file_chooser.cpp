#include <vector>
#include <string>

extern "C" {
#include "ff.h"
}

#include "file_chooser.h"
#include "overlay.h"
#include "utils.h"

#define PAGESIZE OVERLAY_ROWS
#define TOPLINE OVERLAY_TOP

// void FileChooser::set_fs(FATFS *fs) {
//     this->fs = fs;
// }

// list files in dir, starting from number `start` and return at most `len` files. `count` is set to total number of files.
bool FileChooser::list_files(string dir, vector<FileEntry> &files, int start, int len, int *count) {
    DIR d;

    if (f_opendir(&d, dir.c_str()) != 0)
        return false;
    files.clear();

    // ".." entry at the top
    if (start == 0 && len > 0) {
        files.push_back({dir == rootdir ? msg_return : "..", true});
    }
    if (start > 0) start--;
    *count = 1;

    FILINFO fno;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0] != 0) {
        (*count)++;
        if (start > 0) {
            start--;
        } else if (files.size() < len)
            files.push_back({fno.fname, fno.fattrib & AM_DIR});
    }
    f_closedir(&d);
    return true;
}

bool FileChooser::choose_file(string &res) {
    int page = 0, pages, total;
    int active = 0;
    vector<FileEntry> files;

    while (1) {
        overlay_clear();
        bool r = list_files(this->curdir, files, page*PAGESIZE, PAGESIZE, &total);
        if (r) {
            pages = (total+PAGESIZE-1) / PAGESIZE;
            if (active > files.size()-1)
                active = files.size()-1;

            // path (without the drive) and page in the frame's top edge
            char page_str[16];
            snprintf(page_str, sizeof(page_str), "%d/%d", page+1, pages);
            overlay_frame(curdir.substr(curdir.find(':')+1).c_str(), page_str);
            overlay_hints(pages > 1 ? GLYPH_A GLYPH_B " Select  " GLYPH_LEFT GLYPH_RIGHT " Page"
                                    : GLYPH_A GLYPH_B " Select");

            // highlight bar over columns 1-30: icon in 2, name in 4-29, scroll markers in 30
            for (int i = 0; i < files.size(); i++) {
                overlay_cursor(2, i+TOPLINE);
                if (page == 0 && i == 0 && curdir == rootdir)       // return to main menu
                    overlay_printf("%s", overlay_fit(files[i].name, 28).c_str());
                else
                    overlay_printf("%s %s", files[i].is_dir ? GLYPH_FOLDER : GLYPH_FILE,
                                   overlay_fit(files[i].name, 26).c_str());
            }
            if (page > 0) {
                overlay_cursor(30, TOPLINE);
                overlay_printf(GLYPH_UP);
            }
            if (page < pages-1) {
                overlay_cursor(30, TOPLINE+PAGESIZE-1);
                overlay_printf(GLYPH_DOWN);
            }
            delay(300);
            while (1) {
                int r = joy_choice(TOPLINE, files.size(), &active, OSD_KEY_CODE);
                if (r == 1 || r == 4) {
                    if (curdir == rootdir && page == 0 && active == 0) {// return to main menu
                        return false;
                    } else if (files[active].is_dir) {          // enter dir
                        if (page == 0 && active == 0) {         // return to parent dir
                            curdir = curdir.substr(0, curdir.find_last_of("/"));
                        } else {								// enter sub dir
                            curdir += "/" + files[active].name;
                        }
                        active = 0;
                        page = 0;
                        break;
                    } else {                                    // file chosen
                        res = curdir + "/" + files[active].name;
                        return true;
                    }
                } else if (r == 2 && pages > 1) {      // next page, wraps to the first
                    page = (page + 1) % pages;
                    break;
                } else if (r == 3 && pages > 1) {      // previous page, wraps to the last
                    page = (page + pages - 1) % pages;
                    break;
                }
                delay(10);
            }
        } else {
            overlay_status("Error opening directory %d", r);
            return false;
        }
    }
}
