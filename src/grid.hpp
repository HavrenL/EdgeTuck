#pragma once
#include <algorithm>
#include <cmath>

namespace edge {
inline int grid_cells(int pixels, int pitch) { return std::max(0, pixels / std::max(1, pitch)); }
// Count whole cells from the same origin used by the leading edges. Any final
// partial cell stays against the screen edge, outside the drawer's icon columns.
inline int trailing_grid_start(int origin, int end, int pitch, int cells) {
    const int step=std::max(1,pitch), count=grid_cells(end-origin,step);
    return origin+(count-std::clamp(cells,0,count))*step;
}
inline int snap_cells(int delta, int pitch) { return static_cast<int>(std::lround(static_cast<double>(delta) / std::max(1, pitch))); }
// The outer drawer still snaps to the desktop grid. Its compact heading does
// not reserve a desktop icon row; painting and hit testing share these cells.
struct ContentGrid {
    int width{}, height{}, cell_width{}, cell_height{}, header_height{};
    int columns() const { return std::max(1, grid_cells(width, cell_width)); }
    int rows() const { return std::max(0, grid_cells(height-top(), cell_height)); }
    int top() const { return std::clamp(header_height,0,height); }
    int bottom() const { return top()+rows()*cell_height; }
    int hit(int x, int y, int scroll, int items) const {
        if (x<0 || x>=columns()*cell_width || y<top() || y>=std::min(height,bottom())) return -1;
        const int item=((y-top())/cell_height+scroll)*columns()+x/cell_width;
        return item<items ? item : -1;
    }
};
}
