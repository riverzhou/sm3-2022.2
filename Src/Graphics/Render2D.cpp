/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2011-2012 Bart Trzynadlowski, Nik Henson 
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free 
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/
 
/*
 * Render2D.cpp
 *
 * Implementation of the CRender2D class: OpenGL tile generator graphics. 
 *
 * To-Do List
 * ----------
 * - Is there a better way to handle the overscan regions in wide screen mode? 
 *   Is clearing two thin viewports better than one big clear?
 * - Layer priorities in Spikeout attract mode might not be totally correct.
 * - Are v-scroll values 9 or 10 bits? (Does it matter?) Lost World seems to
 *   have some scrolling issues.
 * - A proper shut-down function is needed! OpenGL might not be available when
 *   the destructor for this class is called.
 *
 * Tile Generator Hardware Overview
 * --------------------------------
 *
 * Model 3's medium resolution tile generator hardware appears to be derived
 * from the Model 2 and System 24 chipset, but is much simpler. It consists of
 * four 64x64 tile layers, comprised of 8x8 pixel tiles, with configurable 
 * priorities. There may be additional features but so far, no known Model 3 
 * games use them.
 *
 * VRAM is comprised of 1 MB for tile data and an additional 128 KB for the 
 * palette (each color occupies 32 bits). The four tilemap layers are referred
 * to as: A (0), A' (1), B (2), and B' (3). Palette RAM may be located on a 
 * separate RAM IC.
 *
 * Registers
 * ---------
 *
 * Registers are listed by their byte offset in the PowerPC address space. Each
 * is 32 bits wide and little endian. Only those registers relevant to 
 * rendering are listed here (see CTileGen for others).
 *
 *    Offset:   Description:
 *
 *    0x20    Layer configuration
 *    0x40    Layer A/A' color offset
 *    0x44    Layer B/B' color offset
 *    0x60    Layer A scroll
 *    0x64    Layer A' scroll
 *    0x68    Layer B scroll
 *    0x6C    Layer B' scroll
 *
 * Layer configuration is formatted as:
 *
 *    31                                     0
 *     ???? ???? ???? ???? pqrs tuvw ???? ????
 *
 * Bits 'pqrs' control the color depth of layers B', B, A', and A, 
 * respectively. If set, the layer's pattern data is encoded as 4 bits,
 * otherwise the pixels are 8 bits. Bits 'tuvw' form a 4-bit priority code. The
 * other bits are unused or unknown.
 *
 * The remaining registers are described where appropriate further below.
 *
 * VRAM Memory Map
 * ---------------
 *
 * The lower 1 MB of VRAM is used for storing tiles, per-line horizontal scroll
 * values, and the stencil mask, which determines which of each pair of layers
 * is displayed on a given line and column.
 *
 *    00000-F5FFF   Tile pattern data
 *    F6000-F63FF   Layer A horizontal scroll table (512 lines)
 *    F6400-F67FF   Layer A' horizontal scroll table  
 *    F6800-F6BFF   Layer B horizontal scroll table
 *    F6C00-F6FFF   Layer B' horizontal scroll table
 *    F7000-F77FF   Mask table (assuming 4 bytes per line, 512 lines)
 *    F7800-F7FFF   ?
 *    F8000-F9FFF   Layer A name table
 *    FA000-FBFFF   Layer A' name table
 *    FC000-FDFFF   Layer B name table
 *    FE000-FFFFF   Layer B' name table
 *
 * Tiles may actually address the entire 1 MB space, although in practice,
 * that would conflict with the other fixed memory regions.
 * 
 * Palette
 * -------
 *
 * The palette stores 32768 colors. Each entry is a little endian 32-bit word.
 * The upper 16 bits are unused and the lower 16 bits contain the color:
 *
 *    15                 0
 *     tbbb bbgg gggr rrrr
 *
 * The 't' bit is for transparency. When set, pixels of that color are
 * transparent, unless they are the bottom-most layer.
 *
 * Tile Name Table and Pattern Layout
 * ----------------------------------
 *
 * The name table is a 64x64 array of 16-bit words serving as indices for tile
 * pattern data and the palette. The first 64 words correspond to the first 
 * row of tiles, the next 64 to the second row, etc. Although 64x64 entries
 * describes a 512x512 pixel screen, only the upper-left 62x48 tiles are
 * visible when the vertical and horizontal scroll values are 0. Scrolling 
 * moves the 496x384 pixel 'window' around, with individual wrapping of the
 * two axes.
 *
 * The data is actually arranged in 32-bit chunks in little endian format, so 
 * that tiles 0, 1, 2, and 3 will be stored as 1, 0, 3, 2. Fetching two name 
 * table entries as a single 32-bit word places the left tile in the high 16 
 * bits and the right tile in the low 16 bits.
 *
 * The format of a name table entry in 4-bit color mode is:
 *
 *    15                 0
 *     jkpp pppp pppp iiii
 *
 * The pattern index is '0ppp pppp pppi iiij'. Multiplying by 32 yields the
 * offset in VRAM at which the tile pattern data is stored. Note that the MSB 
 * of the name table entry becomes the LSB of the pattern index. This allows 
 * for 32768 4-bit tile patterns, each occupying 32 bytes, which means the 
 * whole 1 MB VRAM space can be addressed.
 *
 * The 4-bit pattern data is stored as 8 32-bit words. Each word stores a row
 * of 8 pixels:
 *
 *    31                                     0
 *     aaaa bbbb cccc dddd eeee ffff gggg hhhh
 *
 * 'a' is the left-most pixel data. These 4-bit values are combined with bits
 * from the name table to form a palette index, which determines the final
 * color. For example, for pixel 'a', the 15-bit color index is:
 *    
 *      14                0
 *     kpp pppp pppp aaaa
 *
 * Note that index bits are re-used to form the palette index, meaning that
 * the pattern address partly determines the color.
 *
 * In 8-bit color mode, the name table entry looks like:
 *
 *    15                 0
 *     ?ppp pppp iiii iiii
 *
 * The low 15 'p' and 'i' bits together form the pattern index, which must be 
 * multiplied by 64 to get the offset. The pattern data now consists of 16 32-
 * bit words, each containing four 8-bit pixels:
 *
 *    31                                     0
 *     aaaa aaaa bbbb bbbb cccc cccc dddd dddd
 *
 * 'a' is the left-most pixel. Each line is therefore comprised of two 32-bit
 * words. The palette index for pixel 'a' is now formed from:
 *
 *    14                0
 *     ppp pppp aaaa aaaa
 *
 * Stencil Mask 
 * ------------
 *
 * For any pixel position, there are in fact only two visible layers, despite
 * there being four defined layers. The layers are grouped in pairs: A (the
 * 'primary' layer) and A' (the 'alternate') form one pair, and B and B' form
 * the other. Only one of the primary or alternate layers from each group may
 * be visible at a given position. The 'stencil mask' controls this.
 *
 * The mask table is a bit field organized into 512 (or 384?) lines with each
 * bit controlling four columns (32 pixels). The mask does not appear to be
 * affected by scrolling -- that is, it does not scroll with the underlying
 * tiles, which do so independently. The mask remains fixed. Caveat: a bug in
 * Scud Race's 'ROLLING START' animation may indicate this is either not 
 * strictly true or that the upper-left corner of the mask needs to be adjusted
 * slightly. This bug has not been investigated thoroughly yet.
 *
 * Each mask entry is a little endian 32-bit word. The high 16 bits control
 * A/A' and the low 16 bits control B/B'. Each word controls an entire line
 * (32 pixels per bit, 512 pixels per 16-bit line mask, where the first 16
 * pixels are allocated to the overscan region.) If a bit is set to 1, the
 * pixel from the primary layer is used, otherwise the alternate layer is
 * used when the mask is 0. It is important to remember that the layers may
 * have been scrolled independently. The mask operates on the final resultant
 * two pixels that are determined for each location.
 *
 * Example of a line mask:
 *
 *    31                  15                 0
 *     0111 0000 0000 1111 0000 0000 1111 1111
 *
 * These settings would display layer A' for the first 32 pixels of the line,
 * followed by layer A for the next 96 pixels, A' for the subsequent 256 
 * pixels, and A for the final 128 pixels. The first 256 pixels of the line
 * would display layer B' and the second 256 pixels would be from layer B.
 *
 * The stencil mask does not affect layer priorities, which are managed 
 * separately regardless of mask settings.
 *
 * The formula for mapping a screen pixel (0-495) to stencil bit mask is:
 *
 *    bit = 1 << (15 - ((x + 16) / 32))
 *
 * Scrolling
 * ---------
 *
 * Each of the four layers can be scrolled independently. Vertical scroll
 * values are stored in the appropriate scroll register and horizontal scroll
 * values can be sourced either from the register (in which case the entire
 * layer will be scrolled uniformly) or from a table in VRAM (which contains
 * independent values for each line). 
 *
 * The scroll registers are laid out as:
 *
 *    31                                     0
 *     e??? ???y yyyy yyyy h??? ??xx xxxx xxxx
 *
 * The 'e' bit enables the layer when set. The 'y' bits comprise a vertical 
 * scroll value in pixels. The 'x' bits form a horizontal scroll value. If 'h'
 * is set, then the VRAM table (line-by-line scrolling) is used, otherwise the
 * 'x' values are applied to every line. It is also possible that the scroll 
 * values use more or less bits, but probably no more than 1.
 *
 * Each line must be wrapped back to the beginning of the same line. Likewise,
 * vertical scrolling wraps around back to the top of the tilemap.
 *
 * The horizontal scroll table is a series of 16-bit little endian words, one
 * for each line beginning at 0. It appears all the values can be used for
 * scrolling (no control bits have been observed). The number of bits actually
 * used by the hardware is irrelevant -- wrapping has the effect of making 
 * higher order bits unimportant.
 *
 * Layer Priorities
 * ----------------
 *
 * The layer control register (0x20) contains 4 bits that appear to control
 * layer priorities. It is assumed that the 3D graphics, output by the Real3D
 * pixel processors independently of the tile generator, constitute their own
 * 'layer' and that the 2D tilemaps appear in front or behind. There may be a
 * specific function for each priority bit or the field may be interpreted as a
 * single 4-bit value denoting preset layer orders.
 *
 * Color Offsets
 * -------------
 *
 * Color offsets can be applied to the final RGB color value of every pixel.
 * This is used for effects such as fading to a certain color, lightning (Lost
 * World), etc. The current best guess is that the two registers control each
 * pair (A/A' and B/B') of layers. The format appears to be:
 *
 *    31                                     0
 *     ???? ???? rrrr rrrr gggg gggg bbbb bbbb
 *
 * Where 'r', 'g', and 'b' appear to be signed 8-bit color offsets. Because
 * they exceed the color resolution of the palette, they must be scaled
 * appropriately.
 *
 * Color offset registers are handled in TileGen.cpp. Two palettes are computed
 * -- one for A/A' and another for B/B'. These are passed to the renderer.
 */

#include <cstring>
#include "Pkgs/glew.h"
#include "Supermodel.h"
#include "Graphics/Shaders2D.h" // fragment and vertex shaders


/******************************************************************************
 Definitions and Constants
******************************************************************************/

// Shader program files (for use in development builds only)
#define VERTEX_2D_SHADER_FILE "Src/Graphics/Vertex2D.glsl"
#define FRAGMENT_2D_SHADER_FILE "Src/Graphics/Fragment2D.glsl"


/******************************************************************************
 Layer Rendering

 This code is quite slow and badly needs to be optimized. Dirty rectangles
 should be implemented first and tile pre-decoding second.
******************************************************************************/

template <int bits, bool alphaTest, bool clip>
static inline void DrawTileLine(uint32_t *line, int pixelOffset, uint16_t tile, int patternLine, const uint32_t *vram, const uint32_t *palette, uint16_t mask)
{
  static_assert(bits == 4 || bits == 8, "Tiles are either 4- or 8-bit");

  // For 8-bit pixels, each line of tile pattern is two words
  if (bits == 8)
    patternLine *= 2;

  // Compute offset of pattern for this line
  int patternOffset;
  if (bits == 4)
  {
    patternOffset = ((tile & 0x3FFF) << 1) | ((tile >> 15) & 1);
    patternOffset *= 32;
    patternOffset /= 4;
  }
  else
  {
    patternOffset = tile & 0x3FFF;
    patternOffset *= 64;
    patternOffset /= 4;
  } 

  // Name table entry provides high color bits
  uint32_t colorHi = tile & ((bits == 4) ? 0x7FF0 : 0x7F00);

  // Draw
  if (bits == 4)
  {
    uint32_t pattern = vram[patternOffset + patternLine];
    for (int p = 7; p >= 0; p--)
    {
      if (!clip || (clip && pixelOffset >= 0 && pixelOffset < 496))
      {
        uint16_t maskTest = 1 << (15-((pixelOffset+16)/32));  // first 16 pixels in stencil mask are overscan
        bool visible = (mask & maskTest) != 0;
        uint32_t pixel = palette[((pattern >> (p*4)) & 0xF) | colorHi];
        if (alphaTest)
        {
          if (visible && (pixel >> 24) != 0)  // only draw opaque pixels
            line[pixelOffset] = pixel;
        }
        else
        {
          if (visible)
            line[pixelOffset] = pixel;
          else
            line[pixelOffset] = 0;
        }
      }
      ++pixelOffset;
    }
  }
  else
  {
    for (int i = 0; i < 2; i++) // 4 pixels per word
    {
      uint32_t pattern = vram[patternOffset + patternLine + i];
      for (int p = 3; p >= 0; p--)
      {
        if (!clip || (clip && pixelOffset >= 0 && pixelOffset < 496))
        {
          uint16_t maskTest = 1 << (15-((pixelOffset+16)/32));
          bool visible = (mask & maskTest) != 0;
          uint32_t pixel = palette[((pattern >> (p*8)) & 0xFF) | colorHi];
          if (alphaTest)
          {
            if (visible && (pixel >> 24) != 0)
              line[pixelOffset] = pixel;
          }
          else
          {
            if (visible)
              line[pixelOffset] = pixel;
            else
              line[pixelOffset] = 0;  // transparent
          }
        }
        ++pixelOffset;
      }
    }
  }
}

static inline void ClearLayer(uint32_t *pixels)
{
  memset(pixels, 0, 496*384*sizeof(uint32_t));
}

template <int bits, bool alphaTest>
static void DrawLayer(uint32_t *pixels, int layerNum, const uint32_t *vram, const uint32_t *regs, const uint32_t *palette)
{
  const uint16_t *nameTableBase = (const uint16_t *) &vram[(0xF8000 + layerNum * 0x2000) / 4];
  const uint16_t *hScrollTable = (const uint16_t *) &vram[(0xF6000 + layerNum * 0x400) / 4];
  bool lineScrollMode = (regs[0x60/4 + layerNum] & 0x8000) != 0;
  int hFullScroll = regs[0x60/4 + layerNum] & 0x3FF;
  int vScroll = (regs[0x60/4 + layerNum] >> 16) & 0x1FF;

  const uint16_t  *maskTable = (const uint16_t *) &vram[0xF7000 / 4];
  if (layerNum < 2) // little endian: layers A and A' use second word in each pair
    maskTable += 1;

  // If mask bit is clear, alternate layer is shown. We want to test for non-
  // zero, so we flip the mask when drawing alternate layers (layers 1 and 3).
  const uint16_t maskPolarity = (layerNum & 1) ? 0xFFFF : 0x0000;
      
  uint32_t *line = pixels;

  for (int y = 0; y < 384; y++)
  {
    int hScroll = lineScrollMode ? hScrollTable[y] : hFullScroll;
    int hTile = hScroll / 8;
    int hFine = hScroll & 7;        // horizontal pixel offset within tile line
    int vFine = (y + vScroll) & 7;  // vertical pixel offset within 8x8 tile
    const uint16_t *nameTable = &nameTableBase[(64 * ((y + vScroll) / 8)) & 0xFFF]; // clamp to 64x64 = 0x1000
    uint16_t mask = *maskTable ^ maskPolarity;  // each bit covers 32 pixels

    int pixelOffset = -hFine;
    int extraTile = (hFine != 0) ? 1 : 0; // h-scrolling requires part of 63rd tile

    int tx = 0;
    DrawTileLine<bits, alphaTest, true>(line, pixelOffset, nameTable[(hTile ^ 1) & 63], vFine, vram, palette, mask);
    ++hTile;
    pixelOffset += 8;
    for (tx = 1; tx < (62 - 1 + extraTile); tx++)
    {
      DrawTileLine<bits, alphaTest, false>(line, pixelOffset, nameTable[(hTile ^ 1) & 63], vFine, vram, palette, mask);
      ++hTile;
      pixelOffset += 8;
    }
    DrawTileLine<bits, alphaTest, true>(line, pixelOffset, nameTable[(hTile ^ 1) & 63], vFine, vram, palette, mask);
    ++hTile;
    pixelOffset += 8;

    // Advance one line
    maskTable += 2;
    line += 496;
  }
}

bool CRender2D::DrawTilemaps(uint32_t *pixelsBottom, uint32_t *pixelsTop)
{
  unsigned priority = (m_regs[0x20/4] >> 8) & 0xF;
  
  // Render bottom layers
  bool nothingDrawn = true;
  for (int layerNum = 3; layerNum >= 0; layerNum--)
  {
    bool is4Bit = (m_regs[0x20/4] & (1 << (12 + layerNum))) != 0;
    bool enabled = (m_regs[0x60/4 + layerNum] & 0x80000000) != 0;
    bool selected = (priority & (1 << layerNum)) == 0;
    if (enabled && selected)
    {
      if (nothingDrawn)
      {
        if (is4Bit)
          DrawLayer<4, false>(pixelsBottom, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
        else
          DrawLayer<8, false>(pixelsBottom, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
      }
      else
      {
        if (is4Bit)
          DrawLayer<4, true>(pixelsBottom, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
        else
          DrawLayer<8, true>(pixelsBottom, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
      }
      nothingDrawn = false;
    }
  }

  if (nothingDrawn)
    ClearLayer(pixelsBottom);

  // Render top layers
  nothingDrawn = true;
  for (int layerNum = 3; layerNum >= 0; layerNum--)
  {
    bool is4Bit = (m_regs[0x20/4] & (1 << (12 + layerNum))) != 0;
    bool enabled = (m_regs[0x60/4 + layerNum] & 0x80000000) != 0;
    bool selected = (priority & (1 << layerNum)) != 0;
    if (enabled && selected)
    {
      if (nothingDrawn)
      {
        if (is4Bit)
          DrawLayer<4, false>(pixelsTop, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
        else
          DrawLayer<8, false>(pixelsTop, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
      }
      else
      {
        if (is4Bit)
          DrawLayer<4, true>(pixelsTop, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
        else
          DrawLayer<8, true>(pixelsTop, layerNum, m_vram, m_regs, m_palette[layerNum / 2]);
      }
      nothingDrawn = false;
    }
  }
  
  if (nothingDrawn)
    ClearLayer(pixelsTop);

  // Indicate whether color buffer must be cleared because of no bottom layer
  return nothingDrawn;
}


/******************************************************************************
 Frame Display Functions
******************************************************************************/

// Draws a surface to the screen (0 is top and 1 is bottom)
void CRender2D::DisplaySurface(int surface, GLfloat z)
{ 
  // Draw the surface
  float width = m_npot ? 1.0f : (496.0f / 512.0f);
  float height = m_npot ? 1.0f : (384.0f / 512.0f);
  glActiveTexture(GL_TEXTURE0); // texture unit 0
  glBindTexture(GL_TEXTURE_2D, m_texID[surface]);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, 0.0f);     glVertex3f(0.0f, 0.0f, z);
  glTexCoord2f(width, 0.0f);    glVertex3f(1.0f, 0.0f, z);
  glTexCoord2f(width, height);  glVertex3f(1.0f, 1.0f, z);
  glTexCoord2f(0.0f, height);   glVertex3f(0.0f, 1.0f, z);
  glEnd();
}

// Set up viewport and OpenGL state for 2D rendering (sets up blending function but disables blending)
void CRender2D::Setup2D(bool isBottom, bool clearAll)
{
  // Enable texture mapping and blending
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // alpha of 1.0 is opaque, 0 is transparent
  glDisable(GL_BLEND);

  // Disable Z-buffering
  glDisable(GL_DEPTH_TEST);
  
  // Shader program
  glUseProgram(m_shaderProgram);
  
  // Clear everything if requested or just overscan areas for wide screen mode
  if (clearAll)
  {
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glViewport(0, 0, m_totalXPixels, m_totalYPixels);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  else if (isBottom && g_Config.wideScreen)
  {
    // For now, clear w/ black (may want to use color 0 later)
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glViewport(0, 0, m_xOffset, m_totalYPixels);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(m_xOffset + m_xPixels, 0, m_totalXPixels, m_totalYPixels);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  // Set up the viewport and orthogonal projection
  glViewport(m_xOffset, m_yOffset, m_xPixels, m_yPixels);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0.0, 1.0, 1.0, 0.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

// Bottom layers
void CRender2D::BeginFrame(void)
{
  // Update all layers
  bool clear = DrawTilemaps(m_bottomSurface, m_topSurface);
  glActiveTexture(GL_TEXTURE0); // texture unit 0
  glBindTexture(GL_TEXTURE_2D, m_texID[0]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 496, 384, GL_RGBA, GL_UNSIGNED_BYTE, m_topSurface);
  glBindTexture(GL_TEXTURE_2D, m_texID[1]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 496, 384, GL_RGBA, GL_UNSIGNED_BYTE, m_bottomSurface);
  
  // Display bottom surface
  Setup2D(true, clear);
  if (!clear)
    DisplaySurface(1, 0.0);
}

// Top layers
void CRender2D::EndFrame(void)
{
  // Display top surface
  Setup2D(false, false);
  glEnable(GL_BLEND);
  DisplaySurface(0, -0.5);
}


/******************************************************************************
 Emulation Callbacks
******************************************************************************/

// Deprecated
void CRender2D::WriteVRAM(unsigned addr, uint32_t data)
{
}


/******************************************************************************
 Configuration, Initialization, and Shutdown
******************************************************************************/

void CRender2D::AttachRegisters(const uint32_t *regPtr)
{
  m_regs = regPtr;
  DebugLog("Render2D attached registers\n");
}

void CRender2D::AttachPalette(const uint32_t *palPtr[2])
{
  m_palette[0] = palPtr[0];
  m_palette[1] = palPtr[1];
  DebugLog("Render2D attached palette\n");
}

void CRender2D::AttachVRAM(const uint8_t *vramPtr)
{
  m_vram = (uint32_t *) vramPtr;
  DebugLog("Render2D attached VRAM\n");
}

// Memory pool and offsets within it
#define MEMORY_POOL_SIZE      (2*512*384*4)
#define OFFSET_TOP_SURFACE    0             // 512*384*4 bytes
#define OFFSET_BOTTOM_SURFACE (512*384*4)   // 512*384*4

bool CRender2D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes)
{
  // Load shaders
  if (OKAY != LoadShaderProgram(&m_shaderProgram, &m_vertexShader, &m_fragmentShader, 0, 0, s_vertexShaderSource, s_fragmentShaderSource))
    return FAIL;
  
  // Get locations of the uniforms
  glUseProgram(m_shaderProgram);    // bind program
  m_textureMapLoc = glGetUniformLocation(m_shaderProgram, "textureMap");
  glUniform1i(m_textureMapLoc, 0);  // attach it to texture unit 0
  
  // Allocate memory for layer surfaces
  m_memoryPool = new(std::nothrow) uint8_t[MEMORY_POOL_SIZE];
  if (NULL == m_memoryPool)
    return ErrorLog("Insufficient memory for tilemap surfaces (need %1.1f MB).", float(MEMORY_POOL_SIZE) / 0x100000);
  memset(m_memoryPool, 0, MEMORY_POOL_SIZE);  // clear textures
  
  // Set up pointers to memory regions
  m_topSurface    = (uint32_t *) &m_memoryPool[OFFSET_TOP_SURFACE];
  m_bottomSurface = (uint32_t *) &m_memoryPool[OFFSET_BOTTOM_SURFACE];
  
  // Resolution
  m_xPixels = xRes;
  m_yPixels = yRes;
  m_xOffset = xOffset;
  m_yOffset = yOffset;
  m_totalXPixels = totalXRes;
  m_totalYPixels = totalYRes;

  // Create textures
  m_npot = glewIsSupported("GL_ARB_texture_non_power_of_two") != 0;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glGenTextures(2, m_texID);
  for (int i = 0; i < 2; i++)
  {
    glActiveTexture(GL_TEXTURE0); // texture unit 0
    glBindTexture(GL_TEXTURE_2D, m_texID[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    int width = m_npot ? 496 : 512;
    int height = m_npot ? 384 : 512;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_topSurface);
    if (glGetError() != GL_NO_ERROR)
      return ErrorLog("OpenGL was unable to provide %dx%d-texel texture maps for tilemap layers.", width, height);
  }

  DebugLog("Render2D initialized (allocated %1.1f MB)\n", float(MEMORY_POOL_SIZE) / 0x100000);
  return OKAY;
}

CRender2D::CRender2D(void)
{
  DebugLog("Built Render2D\n");
}

CRender2D::~CRender2D(void)
{
  DestroyShaderProgram(m_shaderProgram, m_vertexShader, m_fragmentShader);
  glDeleteTextures(2, m_texID);
  
  if (m_memoryPool)
  {
    delete [] m_memoryPool;
    m_memoryPool = 0;
  }
  
  m_vram = 0;
  m_topSurface = 0;
  m_bottomSurface = 0;

  DebugLog("Destroyed Render2D\n");
}
