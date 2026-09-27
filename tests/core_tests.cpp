#include "layout.hpp"
#include "model.hpp"
#include "motion.hpp"
#include "glass_profile.hpp"
#include "grid.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

static int checks = 0;
static void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
int main() {
    using namespace edge;
    try {
        const auto clock_start=MotionClock::time_point{};
        for(float scale:{1.0f,1.25f,1.5f,2.0f}) for(int side=0;side<3;++side) {
            const float width=400*scale,height=300*scale,peek=14*scale;
            const auto closed=closed_offset(side,width,height,peek);
            require(drawer_content_opacity(width,height,closed,peek)==0,"closed edge never exposes foreground content");
            require(drawer_content_opacity(width,height,{},peek)==1,"open drawer retains full content opacity");
            float previous=0;
            for(int step=0;step<=100;++step) {
                const auto opacity=drawer_content_opacity(width,height,offset_at(closed,step/100.0f),peek);
                require(opacity>=previous && opacity<=1,"content fade is monotonic for all edges and DPI scales"); previous=opacity;
            }
        }
        require(std::abs(motion_elapsed(clock_start,clock_start+std::chrono::microseconds(250))-.25f)<.0001f,
            "animation clock retains sub-millisecond precision");
        for(int hz:{60,120,144,240}) {
            float previous=0;
            for(int frame=1;frame*1000.0/hz<180;++frame) {
                const auto now=clock_start+std::chrono::nanoseconds(static_cast<long long>(frame*1000000000.0/hz));
                const auto position=motion_progress(0,1,motion_elapsed(clock_start,now),180);
                require(position>previous,"each high-refresh sample advances the drawer"); previous=position;
            }
        }
        require(trailing_grid_start(0,1920,76,5)==1520,"1920px right drawer starts at the same column boundary as left/top drawers");
        for(int origin : {-1920,0,23}) for(int width : {1366,1920,2560,3840}) for(int pitch : {75,76,95,114,152}) {
            const int total=grid_cells(width,pitch);
            for(int cells=3;cells<=std::min(total,20);++cells) {
                const int start=trailing_grid_start(origin,origin+width,pitch,cells);
                require(start>=origin && (start-origin)%pitch==0,"trailing free edge shares the absolute grid origin");
                require(grid_cells(origin+width-start,pitch)==cells,"remainder must not create an extra icon column");
                require(origin+width-start-cells*pitch>=0 && origin+width-start-cells*pitch<pitch,"remainder stays at the attached screen edge");
                if(cells<total) require(start-trailing_grid_start(origin,origin+width,pitch,cells+1)==pitch,"right depth changes by exactly one grid cell");
                ContentGrid row{origin+width-start,200,pitch,76,38};
                require(row.hit(cells*pitch,50,0,100)==-1,"right remainder is padding, not a clickable phantom column");
            }
        }
        // Compact content headers are independent of outer desktop snapping.
        // Check asymmetric spacing, final full rows and scroll hit testing.
        for (int gx : {75,96,113,150}) for (int gy : {75,100,141,200}) {
            ContentGrid grid{5*gx,4*gy,gx,gy,gy};
            require(grid.columns()==5 && grid.rows()==3 && grid.bottom()==4*gy,"no fractional cell or unused footer row");
            require(grid.hit(0,gy,0,100)==0 && grid.hit(5*gx-1,4*gy-1,0,100)==14,"first and final cell use the same hit grid");
            require(grid.hit(0,gy-1,0,100)==-1 && grid.hit(5*gx,gy,0,100)==-1 && grid.hit(0,4*gy,0,100)==-1,"header and external bounds reject hits");
            require(grid.hit(gx,2*gy,2,100)==16 && grid.hit(gx,2*gy,2,16)==-1,"scroll offset and missing items are respected");
            require(snap_cells(gx/3,gx)==0 && snap_cells(-gx/3,gx)==0,"subcell motion is a no-op in both directions");
            require(snap_cells(2*gx,gx)==2 && snap_cells(-2*gx,gx)==-2,"positive and negative resize snap equally");
            require(grid_cells(5*gx+gx/2,gx)==5,"screen remainder cannot become a partial cell");
            grid.header_height=gy/2;
            require(grid.rows()==3 && grid.top()==gy/2 && grid.bottom()<=4*gy,"compact heading does not reserve a desktop row");
            require(grid.hit(0,grid.top()-1,0,100)==-1 && grid.hit(0,grid.top(),0,100)==0,"compact title boundary agrees with hit testing");
            require(grid.hit(0,grid.bottom()-1,0,100)==10 && grid.hit(0,grid.bottom(),0,100)==-1,"unused trailing pixels cannot activate a hidden item");
        }
        ContentGrid compact{456,364,76,76,38};
        require(compact.rows()==4 && compact.top()==38,"compact drawer gains a fourth icon row");
        ContentGrid tiny{76,20,76,76,38};
        require(tiny.rows()==0 && tiny.hit(0,19,0,10)==-1,"too-small content area never exposes a clipped row");
        // All four optical edges must use equal, inward curvature. Rotation and
        // DPI scaling must preserve the profile, including the feather seam.
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            for (float depth : {.5f, 3.0f, 9.0f, 17.5f, 18.0f, 24.0f}) {
                const float inset_depth=drawer_visual_inset_dip+depth;
                const auto left = glass_profile(inset_depth*scale, 150*scale, 400*scale, 300*scale, scale);
                const auto right = glass_profile((400-inset_depth)*scale, 150*scale, 400*scale, 300*scale, scale);
                const auto top = glass_profile(200*scale, inset_depth*scale, 400*scale, 300*scale, scale);
                const auto bottom = glass_profile(200*scale, (300-inset_depth)*scale, 400*scale, 300*scale, scale);
                require(std::abs(left.x+right.x)<.001f && std::abs(top.y+bottom.y)<.001f, "opposite glass edges are symmetric");
                require(std::abs(left.x-top.y)<.001f && std::abs(left.coverage-bottom.coverage)<.001f, "top and bottom cannot bypass curved material");
                require(left.x>=0 && right.x<=0 && top.y>=0 && bottom.y<=0, "all edges refract inward");
                require(left.y==0 && top.x==0, "straight edges do not shift tangentially");
                if (depth>=18) require(left.coverage==0 && left.x==0, "rim blends to live center without a hard inner ring");
            }
            const float corner_point=(drawer_visual_inset_dip+drawer_corner_dip*.4f)*scale;
            const auto corner = glass_profile(corner_point, corner_point, 400*scale, 300*scale, scale);
            require(corner.x>0 && std::abs(corner.x-corner.y)<.001f, "corner bends continuously along diagonal normal");
            require(glass_profile(0,0,400*scale,300*scale,scale).coverage==0, "outside rounded clip stays transparent");
        }
        // A collapsed drawer leaves exactly one narrow section of the same body;
        // test all docking sides and DPI sizes, not a detached handle model.
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            const float width = 375*scale, height = 294*scale, peek = 14*scale;
            for (int side = 0; side < 3; ++side) {
                const auto hidden = closed_offset(side, width, height, peek);
                const float visible_x = std::max(0.0f, std::min(width, hidden.x+width)-std::max(0.0f, hidden.x));
                const float visible_y = std::max(0.0f, std::min(height, hidden.y+height)-std::max(0.0f, hidden.y));
                require(side == 2 ? visible_y == peek : visible_x == peek, "closed body must expose exactly the peek width");
                const auto shown = offset_at(hidden, 1);
                require(shown.x == 0 && shown.y == 0, "fully open body is flush to screen edge");
            }
        }
        float previous = 0;
        for (int ms = 0; ms <= 200; ++ms) {
            const float now = motion_progress(0, 1, static_cast<float>(ms), 180);
            require(now >= previous && now <= 1, "opening is monotonic and cannot overshoot"); previous = now;
        }
        const float interrupted = motion_progress(0, 1, 63, 180);
        require(motion_progress(interrupted, 0, 0, 180) == interrupted, "reverse begins at visible position");
        require(motion_progress(interrupted, 0, 200, 180) == 0, "reverse ends closed");
        require(motion_progress(0, 1, 8, 180) > 0, "motion has no built-in hover delay");
        std::vector<Slot> slots{{1, 0, 3}, {2, 3, 3}, {3, 6, 3}};
        require(resize_end(slots, 2, 7, 10), "middle expansion");
        require(slots == std::vector<Slot>{{1, 0, 3}, {2, 3, 4}, {3, 7, 3}}, "neighbor must move exactly one cell");
        resize_end(slots, 2, 1000, 10);
        require(slots[1].span == 4 && slots[2].end() == 10, "full edge clamps expansion");
        resize_start(slots, 2, -10, 10);
        require(valid_layout(slots, 10) && slots[1].start == 3, "start resize cannot overlap prior occupied cells");
        slots = {{1, 2, 2}, {2, 6, 2}, {3, 9, 2}};
        resize_start(slots, 3, 5, 12);
        require(slots == std::vector<Slot>{{1, 1, 2}, {2, 3, 2}, {3, 5, 6}}, "start resize pushes backward chain");
        slots = {{1, 0, 2}, {2, 4, 2}, {3, 8, 2}};
        resize_end(slots, 1, 3, 12);
        require(slots[1].start == 4 && slots[2].start == 8, "preserve gaps before collision");
        require(first_gap(slots, 2, 12) == 6, "find free run");
        require(!resize_end(slots, 404, 10, 12), "unknown id is rejected");
        const auto unchanged = slots;
        require(!resize_end(slots, 1, 10, 3) && unchanged == slots, "invalid input leaves layout unchanged");

        std::mt19937 random(117);
        slots = {{1, 1, 3}, {2, 6, 4}, {3, 13, 3}, {4, 19, 2}};
        for (int i = 0; i < 20000; ++i) {
            const int id = 1 + random() % 4, requested = static_cast<int>(random() % 50) - 10;
            switch (random() % 3) {
            case 0: resize_start(slots, id, requested, 24); break;
            case 1: resize_end(slots, id, requested, 24); break;
            case 2: move_slot(slots, id, requested, 24); break;
            }
            require(valid_layout(slots, 24), "randomized operations must preserve non-overlap and bounds");
            for (int j = 0; j < 4; ++j) require(slots[j].id == j + 1 && slots[j].span >= 2, "stable order and minimum span");
        }

        const auto root = std::filesystem::temp_directory_path() / (L"EdgeTuck-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        const auto config = root / L"settings.dat";
        const auto original = root / L"原文件 空格.txt";
        { std::ofstream out(original, std::ios::binary); out << "unchanged bytes\n"; }
        const DWORD attributes = GetFileAttributesW(original.c_str());
        Settings expected = defaults();
        expected.live_background=false;
        expected.responsive_priority=true;
        expected.storage_mode=StorageMode::Directory;
        expected.storage_directory=L"D:\\我的文件\\轻屉收纳";
        expected.material = {3.17f, 27, 1.13f, 1.42f, false, true, false};
        expected.drawers[0].name = L"工作\"区 · 资料";
        expected.drawers[0].items = {original.wstring(), L"C:\\Desktop\\不存在的引用.txt"};
        expected.theme = Theme::Dark; expected.motion = false; expected.hover_ms = 140; expected.close_ms = 400;
        std::wstring error;
        require(save_settings(config, expected, error), "save Unicode references");
        Settings loaded;
        require(load_settings(config, loaded, error), "load Unicode references");
        require(loaded.hover_ms == 0 && loaded.close_ms == 0, "legacy settings adopt immediate hover and leave without resetting references");
        require(loaded.material == expected.material, "custom glass parameters and switches survive restart");
        require(!loaded.live_background,"screenshot-compatible background choice survives restart");
        require(loaded.responsive_priority,"process priority preference survives restart");
        require(loaded.storage_mode==StorageMode::Directory && loaded.storage_directory==expected.storage_directory,"storage mode and Unicode directory survive restart");
        const auto legacy = root / L"legacy.dat";
        { std::ofstream out(legacy, std::ios::binary); out << "EDGETUCK 1\n0 1 1 0 0 1\n1 1 2 4 5 \"Work\" 0\n"; }
        Settings migrated;
        require(load_settings(legacy, migrated, error) && migrated.material == GlassMaterial{} && migrated.drawers[0].name == L"Work", "v1 gains clear glass defaults without changing drawer data");
        require(migrated.live_background,"legacy configuration gains the requested live background default");
        require(!migrated.responsive_priority,"legacy configuration keeps normal priority until enabled");
        require(migrated.storage_mode==StorageMode::Desktop,"legacy configuration keeps desktop storage until explicitly changed");
        { std::ofstream out(legacy,std::ios::binary); out<<R"(EDGETUCK 4
0 1 1 0 0 1
1.2 18 1 0.5 1 1 1
1
7 1 2 4 5 "Work" 1
"C:\\fixtures\\ET_Work\\report.txt"
"C:\\fixtures\\ET_Work" "12:0:456" 1 17 912 0
)"; }
        require(load_settings(legacy,migrated,error) && !migrated.responsive_priority && migrated.drawers[0].folder==L"C:\\fixtures\\ET_Work" && migrated.drawers[0].folder_identity==L"12:0:456" && migrated.drawers[0].restore_y==912 && migrated.drawers[0].items.size()==1,"v4 migration retains bindings, coordinates and file order");
        { std::ofstream out(legacy, std::ios::binary); out << "EDGETUCK 2\n0 1 1 0 0 0\n1.2 18 99 0.5 1 1 1\n"; }
        require(!load_settings(legacy, migrated, error), "out of range shader parameters rejected");
        std::filesystem::remove(legacy);
        require(loaded.theme == Theme::Dark && !loaded.motion && loaded.drawers[0].name == expected.drawers[0].name && loaded.drawers[0].items == expected.drawers[0].items, "configuration round trip");
        require(GetFileAttributesW(original.c_str()) == attributes && std::filesystem::file_size(original) == 16, "reference persistence must not touch original file");
        std::ifstream content(original, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(content)), {}); content.close();
        require(bytes == "unchanged bytes\n", "original content preserved");
        { std::ofstream bad(config, std::ios::binary); bad << "EDGETUCK 999\n"; }
        const auto before = loaded.drawers[0].name;
        require(!load_settings(config, loaded, error) && loaded.drawers[0].name == before, "corrupt config must not partly replace model");
        require(save_settings(config, expected, error), "replace corrupt settings without overwriting a valid backup");
        expected.theme = Theme::Light;
        require(save_settings(config, expected, error), "save second generation and retain backup");
        { std::ofstream bad(config, std::ios::binary); bad << "interrupted write"; }
        require(load_settings(config, loaded, error) && loaded.theme == Theme::Dark && !error.empty(), "recover previous valid generation after interrupted write");
        // Remove only the exact files created by this test; never recursively remove a user path.
        std::filesystem::remove(config); std::filesystem::remove(config.wstring() + L".bak"); std::filesystem::remove(original); std::filesystem::remove(root);
        std::cout << "PASS: " << checks << " checks (grid push, clamping, persistence, original-file preservation)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
