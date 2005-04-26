/**
 *  GameView Component for Lincity-NG.
 *   
 *  February 2005, Wolfgang Becker <uafr@gmx.de>
 *
 */
#include <config.h>

#include "GameView.hpp"

#include "gui/TextureManager.hpp"
#include "gui/Painter.hpp"
#include "gui/Rect2D.hpp"
#include "gui/Color.hpp"
#include "gui/ComponentFactory.hpp"
#include "gui/XmlReader.hpp"
#include "gui/Event.hpp"
#include "gui/PhysfsStream/PhysfsSDL.hpp"

#include "lincity/lin-city.h"
#include "lincity/lctypes.h"
#include "lincity/engglobs.h"

#include "Mps.hpp"
#include "MapEdit.hpp"
#include "MiniMap.hpp"
#include "Dialog.hpp"
#include "Config.hpp"

#include <SDL_keysym.h>
#include <math.h>

#include "gui_interface/shared_globals.h"

const float GameView::defaultTileWidth;
const float GameView::defaultTileHeight;

GameView* gameViewPtr = 0;

GameView* getGameView()
{
    return gameViewPtr;
}
    

GameView::GameView()
{
    assert(gameViewPtr == 0);
    gameViewPtr = this;
    mTextures = SDL_CreateMutex();
    mThreadRunning = SDL_CreateMutex();
    loaderThread = 0;
}

GameView::~GameView()
{
    stopThread = true;
    SDL_mutexP( mThreadRunning );
    SDL_KillThread( loaderThread );
    SDL_DestroyMutex( mThreadRunning );
    SDL_DestroyMutex( mTextures );
    
    for(int i = 0; i < NUM_OF_TYPES; ++i) {
        delete cityTextures[i];
        SDL_FreeSurface( cityImages[i] );
    }
    delete blankTexture;

    if(gameViewPtr == this)
        gameViewPtr = 0;
}

//Static function to use with SDL_CreateThread
int GameView::gameViewThread( void* data )
{
    std::cout << "** Start loading Textures **\n";
    GameView* gv = (GameView*) data;
    gv->loadTextures();
    gv->requestRedraw();
    std::cout << "** Finished loading Textures **\n";
    return 0;
}

void GameView::parse(XmlReader& reader)
{
    //Read from config
    XmlReader::AttributeIterator iter(reader);
    while(iter.next()) {
        const char* attribute = (const char*) iter.getName();
        const char* value = (const char*) iter.getValue();
        
        //check if Attribute handled by parent
        if(parseAttribute(attribute, value)) {
            continue;
        } else {
            std::cerr << "GameView::parse# Skipping unknown attribute '" << attribute << "'.\n";
        }
    }
    // no more elements to parse

    //Load Textures
    blankTexture = readTexture( "blank.png" );
    memset( &cityTextures, 0, sizeof( cityTextures ) );
    memset( &cityImages, 0, sizeof( cityImages ) );
    stopThread = false;
    SDL_mutexP( mThreadRunning );
    loaderThread = SDL_CreateThread( gameViewThread, this );
   
    //GameView is resizable
    setFlags(FLAG_RESIZABLE);

    //start in the centre of the city
      //TODO: change start-position to location from savegame
      //and set these values so they will be stored in savegame.
      //main_screen_originx main_screen_originy
      //std::cout << "main_screen_originx=" << main_screen_originx; 
      //std::cout << " main_screen_originy=" << main_screen_originy << "\n";
    //because on startup the size of this Control is 0
    //we use values from config instead of getWidth() and getHeight())
    //so we can not use zoom( defaultZoom ) likewise
    zoom = 1.0;
    tileWidth = defaultTileWidth * zoom;
    tileHeight = defaultTileHeight * zoom; 
    virtualScreenWidth = tileWidth * WORLD_SIDE_LEN;
    virtualScreenHeight = tileHeight * WORLD_SIDE_LEN;
    viewport.x = floor ( ( virtualScreenWidth - getConfig()->videoX  ) / 2 );
    viewport.y = floor ( ( virtualScreenHeight- getConfig()->videoY  ) / 2 );

    mouseInGameView = false;
    dragging = false;
    roadDragging = false;
    startRoad = MapPoint(0, 0);
    middleButtonDown = false;
    tileUnderMouse = MapPoint(0, 0);
    dragStart = Vector2(0, 0);
    hideHigh = false;
    cursorSize = 0;

    mapOverlay = overlayNone;
    mapMode = MiniMap::NORMAL;
}

/*
 * size in Tiles of marking under Cursor
 * atm 0 is an outlined Version of size 1.
 */
void GameView::setCursorSize( int size )
{
    if( size != cursorSize )    
    {
        cursorSize = size;
        setDirty();
    }
} 

/*
 *  inform GameView about change in Mini Map Mode
 */
void GameView::setMapMode( MiniMap::DisplayMode mMode ) {
    mapMode = mMode;
    if( mapOverlay != overlayNone ){
    requestRedraw();
    }
}

/*
 *  Get Tile in Center of Screen.
 */
MapPoint GameView::getCenter(){
    Vector2 center( getWidth() / 2, getHeight() / 2 );
    return getTile( center ); 
}
    
/*
 * Adjust the Zoomlevel.
 */
void GameView::setZoom(float newzoom){
    MapPoint centerTile  = getCenter(); 
    
    if ( newzoom < .125 ) return;
    if ( newzoom > 4 ) return;
    
    zoom = newzoom;
    
    // fix rounding errors...
    if(fabs(zoom - 1.0) < .01)
        zoom = 1;
    
    tileWidth = defaultTileWidth * zoom;
    tileHeight = defaultTileHeight * zoom;
    //a virtual screen containing the whole city
    virtualScreenWidth = tileWidth * WORLD_SIDE_LEN;
    virtualScreenHeight = tileHeight * WORLD_SIDE_LEN;
    std::cout << "Zoom " << zoom  << "\n";

    //Show the Center
    show( centerTile );
}

/* set Zoomlevel to 100% */
void GameView::resetZoom(){
    setZoom( 1.0 );
}

/* increase Zoomlevel */
void GameView::zoomIn(){
    setZoom( zoom * sqrt( 2 ) );
}

/** decrease Zoomlevel */
void GameView::zoomOut(){
    setZoom( zoom / sqrt( 2 ) );
}

/**
 *  Show City Tile(x/y) by centering the screen 
 */
void GameView::show( MapPoint map )
{    
    Vector2 center;
    center.x = virtualScreenWidth / 2 + ( map.x - map.y ) * ( tileWidth / 2 );
    center.y = ( map.x + map.y ) * ( tileHeight / 2 ) + ( tileHeight / 2 ); 
    
    viewport.x = center.x - ( getWidth() / 2 );
    viewport.y = center.y - ( getHeight() / 2 );
    requestRedraw();
}

/*
 * Loads Texture from filename, Returns Pointer to Texture 
 * or Null if no file found.
 */
Texture* GameView::readTexture(const std::string& filename)
{
    std::string nfilename = std::string("images/tiles/") + filename;
    Texture* currentTexture;
    try {
        currentTexture = texture_manager->load(nfilename);
    } catch(std::exception& err) {
        std::cerr << nfilename << "GameView::readTexture# missing: " << err.what() << "\n";
        return 0;
    }
    return currentTexture;
}

/*
 * Loads Image from filename, Returns Pointer to Image
 * or Null if no file found.
 */
SDL_Surface* GameView::readImage(const std::string& filename)
{
    std::string nfilename = std::string("images/tiles/") + filename;
    SDL_Surface* currentImage;
    currentImage = IMG_Load_RW(getPhysfsSDLRWops( nfilename ), 1);
    if( !currentImage ) {
        std::cerr << "GameView::readImage# Could not load image "<< nfilename << "\n";
    }
    return currentImage;
}

/**
 * preload a City Texture and fill in X and Y Data.
 *
 * images/tiles/images.xml contains the x-Coordinate of the 
 * middle of the Building in Case the Image is asymetric,
 * eg. a high tower with a long shadow to the right 
 * 
 *  Some of the Image to Texture Conversion seems not to be threadsave
 *  in OpenGL, so load just Images and convert them to Textures on 
 *  demand in the main Tread. 
 */
void GameView::preReadCityTexture( int textureType, const std::string& filename )
{
    if( stopThread ){ //skip loading if we stop anyway
        return;
    }
    XmlReader reader( "images/tiles/images.xml" );
    int xmlX = -1;
    int xmlY = -1;
    bool hit = false;
    
    SDL_mutexP( mTextures );
    cityImages[ textureType ] = readImage( filename );
    if( cityImages[ textureType ] ) 
    {
        //now we need to find x for our filename in images/tiles/images.xml 

        while( reader.read() ) 
        {
            if( reader.getNodeType() == XML_READER_TYPE_ELEMENT) 
            {
                const std::string& element = (const char*) reader.getName();
                if( element == "image" )
                {
                    XmlReader::AttributeIterator iter(reader);
                    while(iter.next()) 
                    {
                        const char* name = (const char*) iter.getName();
                        const char* value = (const char*) iter.getValue();
                        if( strcmp(name, "file" ) == 0 ) 
                        {
                            if( filename.compare( value ) == 0 ) 
                            {
                                hit = true;
                            }
                        }
                        else if( strcmp(name, "x" ) == 0 )
                        {
                            if(sscanf(value, "%i", &xmlX) != 1) 
                            {
                                std::cerr << "GameView::preReadCityTexture# Error parsing integer value '" << value << "' in x attribute.\n";
                                xmlX = -1;
                            }
                        }
                       else if(strcmp(name, "y") == 0 ) 
                        {
                            if(sscanf(value, "%i", &xmlY) != 1) 
                            {
                                std::cerr << "GameView::preReadCityTexture# Error parsing integer value '" << value << "' in y attribute.\n";
                                xmlY = -1;
                            }
                        }
                    } 
                    if( hit )
                    {
                        break;
                    }
                }
            }
        } 
       
        if( hit && ( xmlX >= 0 ) )
        { 
            cityTextureX[ textureType ] = xmlX;
        }
        else
        {
            cityTextureX[ textureType ] = int( ( cityImages[ textureType ]->w / 2 ) );
        }
        cityTextureY[ textureType ] = int( cityImages[ textureType ]->h ); 
    }
    SDL_mutexV( mTextures );
}
    
/**
 *  Preload all required Textures. (his Function is called by loaderThread)
 *  Some of the Image to Texture Conversion seems not to be threadsave
 *  in OpenGL, so load just Images and convert them to Textures on 
 *  demand in the main Tread. 
 */
void GameView::loadTextures()
{
   //We need Textures for all Types from lincity/lctypes.h 
   //Code Generation:
   /*
       grep -e LCT src/lincity/lctypes.h | sed  \
           -e 's/#define LC/   preReadCityTexture( CS/' \
           -e 's/_G /, /'       \
           -e 's/_G\t/, /'     \
           -e 's/"/.png" );/2'                 
   */
   preReadCityTexture( CST_GREEN, 	"green.png" );
   preReadCityTexture( CST_POWERL_H_L, "powerlhl.png" );
   preReadCityTexture( CST_POWERL_V_L,  	"powerlvl.png" );
   preReadCityTexture( CST_POWERL_LD_L, 	"powerlldl.png" );
   preReadCityTexture( CST_POWERL_RD_L, 	"powerlrdl.png" );
   preReadCityTexture( CST_POWERL_LU_L,  	"powerllul.png" );
   preReadCityTexture( CST_POWERL_RU_L,  	"powerlrul.png" );
   preReadCityTexture( CST_POWERL_LDU_L, "powerlldul.png" );
   preReadCityTexture( CST_POWERL_LDR_L, "powerlldrl.png" );
   preReadCityTexture( CST_POWERL_LUR_L, "powerllurl.png" );
   preReadCityTexture( CST_POWERL_UDR_L, "powerludrl.png" );
   preReadCityTexture( CST_POWERL_LUDR_L, "powerlludrl.png" );
   preReadCityTexture( CST_POWERL_H_D,        "powerlhd.png" );
   preReadCityTexture( CST_POWERL_V_D,        "powerlvd.png" );
   preReadCityTexture( CST_POWERL_LD_D,       "powerlldd.png" );
   preReadCityTexture( CST_POWERL_RD_D,       "powerlrdd.png" );
   preReadCityTexture( CST_POWERL_LU_D,       "powerllud.png" );
   preReadCityTexture( CST_POWERL_RU_D,       "powerlrud.png" );
   preReadCityTexture( CST_POWERL_LDU_D,      "powerlldud.png" );
   preReadCityTexture( CST_POWERL_LDR_D,      "powerlldrd.png" );
   preReadCityTexture( CST_POWERL_LUR_D,      "powerllurd.png" );
   preReadCityTexture( CST_POWERL_UDR_D,      "powerludrd.png" );
   preReadCityTexture( CST_POWERL_LUDR_D,     "powerlludrd.png" );
   preReadCityTexture( CST_SHANTY,            "shanty.png" );
   preReadCityTexture( CST_POWERS_SOLAR, "powerssolar.png" );
   preReadCityTexture( CST_POWERS_COAL_EMPTY, "powerscoal-empty.png" );
   preReadCityTexture( CST_POWERS_COAL_LOW,   "powerscoal-low.png" );
   preReadCityTexture( CST_POWERS_COAL_MED,   "powerscoal-med.png" );
   preReadCityTexture( CST_POWERS_COAL_FULL,  "powerscoal-full.png" );
   preReadCityTexture( CST_BURNT, 	"burnt_land.png" );
   preReadCityTexture( CST_SUBSTATION_R, "substation-R.png" );
   preReadCityTexture( CST_SUBSTATION_G,      "substation-G.png" );
   preReadCityTexture( CST_SUBSTATION_RG,     "substation-RG.png" );
   preReadCityTexture( CST_UNIVERSITY, "university.png" );
   preReadCityTexture( CST_RESIDENCE_LL, "reslowlow.png" );
   preReadCityTexture( CST_RESIDENCE_ML, "resmedlow.png" );
   preReadCityTexture( CST_RESIDENCE_HL, "reshilow.png" );
   preReadCityTexture( CST_RESIDENCE_LH,      "reslowhi.png" );
   preReadCityTexture( CST_RESIDENCE_MH,      "resmedhi.png" );
   preReadCityTexture( CST_RESIDENCE_HH,      "reshihi.png" );
   preReadCityTexture( CST_MARKET_EMPTY, "market-empty.png" );
   preReadCityTexture( CST_MARKET_LOW,        "market-low.png" );
   preReadCityTexture( CST_MARKET_MED,        "market-med.png" );
   preReadCityTexture( CST_MARKET_FULL,       "market-full.png" );
   preReadCityTexture( CST_RECYCLE, 	"recycle-centre.png" );
   preReadCityTexture( CST_TRACK_LR, 	"tracklr.png" );
   preReadCityTexture( CST_TRACK_LU,          "tracklu.png" );
   preReadCityTexture( CST_TRACK_LD,          "trackld.png" );
   preReadCityTexture( CST_TRACK_UD,          "trackud.png" );
   preReadCityTexture( CST_TRACK_UR,          "trackur.png" );
   preReadCityTexture( CST_TRACK_DR,          "trackdr.png" );
   preReadCityTexture( CST_TRACK_LUR,         "tracklur.png" );
   preReadCityTexture( CST_TRACK_LDR,         "trackldr.png" );
   preReadCityTexture( CST_TRACK_LUD,         "tracklud.png" );
   preReadCityTexture( CST_TRACK_UDR,         "trackudr.png" );
   preReadCityTexture( CST_TRACK_LUDR,        "trackludr.png" );
   preReadCityTexture( CST_PARKLAND_PLANE, "parkland-plane.png" );
   preReadCityTexture( CST_PARKLAND_LAKE, "parkland-lake.png" );
   preReadCityTexture( CST_MONUMENT_0, "monument0.png" );
   preReadCityTexture( CST_MONUMENT_1,        "monument1.png" );
   preReadCityTexture( CST_MONUMENT_2,        "monument2.png" );
   preReadCityTexture( CST_MONUMENT_3,        "monument3.png" );
   preReadCityTexture( CST_MONUMENT_4,        "monument4.png" );
   preReadCityTexture( CST_MONUMENT_5,        "monument5.png" );
   preReadCityTexture( CST_COALMINE_EMPTY, "coalmine-empty.png" );
   preReadCityTexture( CST_COALMINE_LOW, "coalmine-low.png" );
   preReadCityTexture( CST_COALMINE_MED, "coalmine-med.png" );
   preReadCityTexture( CST_COALMINE_FULL, "coalmine-full.png" );
   preReadCityTexture( CST_RAIL_LR,          "raillr.png" );
   preReadCityTexture( CST_RAIL_LU,          "raillu.png" );
   preReadCityTexture( CST_RAIL_LD,          "railld.png" );
   preReadCityTexture( CST_RAIL_UD,          "railud.png" );
   preReadCityTexture( CST_RAIL_UR,          "railur.png" );
   preReadCityTexture( CST_RAIL_DR,          "raildr.png" );
   preReadCityTexture( CST_RAIL_LUR,         "raillur.png" );
   preReadCityTexture( CST_RAIL_LDR,         "railldr.png" );
   preReadCityTexture( CST_RAIL_LUD,         "raillud.png" );
   preReadCityTexture( CST_RAIL_UDR,         "railudr.png" );
   preReadCityTexture( CST_RAIL_LUDR,        "railludr.png" );
   preReadCityTexture( CST_FIRE_1,           "fire1.png" );
   preReadCityTexture( CST_FIRE_2,           "fire2.png" );
   preReadCityTexture( CST_FIRE_3,           "fire3.png" );
   preReadCityTexture( CST_FIRE_4,           "fire4.png" );
   preReadCityTexture( CST_FIRE_5,           "fire5.png" );
   preReadCityTexture( CST_FIRE_DONE1,       "firedone1.png" );
   preReadCityTexture( CST_FIRE_DONE2,       "firedone2.png" );
   preReadCityTexture( CST_FIRE_DONE3,       "firedone3.png" );
   preReadCityTexture( CST_FIRE_DONE4,       "firedone4.png" );
   preReadCityTexture( CST_ROAD_LR,          "roadlr.png" );
   preReadCityTexture( CST_ROAD_LU,          "roadlu.png" );
   preReadCityTexture( CST_ROAD_LD,          "roadld.png" );
   preReadCityTexture( CST_ROAD_UD,          "roadud.png" );
   preReadCityTexture( CST_ROAD_UR,          "roadur.png" );
   preReadCityTexture( CST_ROAD_DR,          "roaddr.png" );
   preReadCityTexture( CST_ROAD_LUR,         "roadlur.png" );
   preReadCityTexture( CST_ROAD_LDR,         "roadldr.png" );
   preReadCityTexture( CST_ROAD_LUD,         "roadlud.png" );
   preReadCityTexture( CST_ROAD_UDR,         "roadudr.png" );
   preReadCityTexture( CST_ROAD_LUDR,        "roadludr.png" );
   preReadCityTexture( CST_OREMINE_5,         "oremine5.png" );
   preReadCityTexture( CST_OREMINE_6,         "oremine6.png" );
   preReadCityTexture( CST_OREMINE_7,         "oremine7.png" );
   preReadCityTexture( CST_OREMINE_8,         "oremine8.png" );
   preReadCityTexture( CST_OREMINE_1, 	"oremine1.png" );
   preReadCityTexture( CST_OREMINE_2, 	"oremine2.png" );
   preReadCityTexture( CST_OREMINE_3, 	"oremine3.png" );
   preReadCityTexture( CST_OREMINE_4, 	"oremine4.png" );
   preReadCityTexture( CST_HEALTH, 	"health.png" );
   preReadCityTexture( CST_SCHOOL, 	"school0.png" );
   preReadCityTexture( CST_EX_PORT, 	"ex_port.png" );
   preReadCityTexture( CST_MILL_0,            "mill0.png" );
   preReadCityTexture( CST_MILL_1,            "mill1.png" );
   preReadCityTexture( CST_MILL_2,            "mill2.png" );
   preReadCityTexture( CST_MILL_3,            "mill3.png" );
   preReadCityTexture( CST_MILL_4,            "mill4.png" );
   preReadCityTexture( CST_MILL_5,            "mill5.png" );
   preReadCityTexture( CST_MILL_6,            "mill6.png" );
   preReadCityTexture( CST_ROCKET_1,          "rocket1.png" );
   preReadCityTexture( CST_ROCKET_2, 	"rocket2.png" );
   preReadCityTexture( CST_ROCKET_3, 	"rocket3.png" );
   preReadCityTexture( CST_ROCKET_4, 	"rocket4.png" );
   preReadCityTexture( CST_ROCKET_5,          "rocket5.png" );
   preReadCityTexture( CST_ROCKET_6,          "rocket6.png" );
   preReadCityTexture( CST_ROCKET_7, 	"rocket7.png" );
   preReadCityTexture( CST_ROCKET_FLOWN, "rocketflown.png" );
   preReadCityTexture( CST_WINDMILL_1_G,      "windmill1g.png" );
   preReadCityTexture( CST_WINDMILL_2_G,      "windmill2g.png" );
   preReadCityTexture( CST_WINDMILL_3_G,      "windmill3g.png" );
   preReadCityTexture( CST_WINDMILL_1_RG,     "windmill1rg.png" );
   preReadCityTexture( CST_WINDMILL_2_RG,     "windmill2rg.png" );
   preReadCityTexture( CST_WINDMILL_3_RG,     "windmill3rg.png" );
   preReadCityTexture( CST_WINDMILL_1_R,      "windmill1r.png" );
   preReadCityTexture( CST_WINDMILL_2_R,      "windmill2r.png" );
   preReadCityTexture( CST_WINDMILL_3_R,      "windmill3r.png" );
   preReadCityTexture( CST_WINDMILL_1_W,      "windmill1w.png" );
   preReadCityTexture( CST_WINDMILL_2_W,      "windmill2w.png" );
   preReadCityTexture( CST_WINDMILL_3_W,      "windmill3w.png" );
   preReadCityTexture( CST_BLACKSMITH_0,        "blacksmith0.png" );
   preReadCityTexture( CST_BLACKSMITH_1,        "blacksmith1.png" );
   preReadCityTexture( CST_BLACKSMITH_2,        "blacksmith2.png" );
   preReadCityTexture( CST_BLACKSMITH_3,        "blacksmith3.png" );
   preReadCityTexture( CST_BLACKSMITH_4,        "blacksmith4.png" );
   preReadCityTexture( CST_BLACKSMITH_5,        "blacksmith5.png" );
   preReadCityTexture( CST_BLACKSMITH_6,        "blacksmith6.png" );
   preReadCityTexture( CST_POTTERY_0,           "pottery0.png" );
   preReadCityTexture( CST_POTTERY_1,           "pottery1.png" );
   preReadCityTexture( CST_POTTERY_2,           "pottery2.png" );
   preReadCityTexture( CST_POTTERY_3,           "pottery3.png" );
   preReadCityTexture( CST_POTTERY_4,           "pottery4.png" );
   preReadCityTexture( CST_POTTERY_5,           "pottery5.png" );
   preReadCityTexture( CST_POTTERY_6,           "pottery6.png" );
   preReadCityTexture( CST_POTTERY_7,           "pottery7.png" );
   preReadCityTexture( CST_POTTERY_8,           "pottery8.png" );
   preReadCityTexture( CST_POTTERY_9,           "pottery9.png" );
   preReadCityTexture( CST_POTTERY_10,          "pottery10.png" );
   preReadCityTexture( CST_WATER,             "water.png" );
   preReadCityTexture( CST_WATER_D,           "waterd.png" );
   preReadCityTexture( CST_WATER_R,           "waterr.png" );
   preReadCityTexture( CST_WATER_U,           "wateru.png" );
   preReadCityTexture( CST_WATER_L,           "waterl.png" );
   preReadCityTexture( CST_WATER_LR,          "waterlr.png" );
   preReadCityTexture( CST_WATER_UD,          "waterud.png" );
   preReadCityTexture( CST_WATER_LD,          "waterld.png" );
   preReadCityTexture( CST_WATER_RD,          "waterrd.png" );
   preReadCityTexture( CST_WATER_LU,          "waterlu.png" );
   preReadCityTexture( CST_WATER_UR,          "waterur.png" );
   preReadCityTexture( CST_WATER_LUD,         "waterlud.png" );
   preReadCityTexture( CST_WATER_LRD,         "waterlrd.png" );
   preReadCityTexture( CST_WATER_LUR,         "waterlur.png" );
   preReadCityTexture( CST_WATER_URD,         "waterurd.png" );
   preReadCityTexture( CST_WATER_LURD,        "waterlurd.png" );
   preReadCityTexture( CST_CRICKET_1,         "cricket1.png" );
   preReadCityTexture( CST_CRICKET_2,         "cricket2.png" );
   preReadCityTexture( CST_CRICKET_3,         "cricket3.png" );
   preReadCityTexture( CST_CRICKET_4,         "cricket4.png" );
   preReadCityTexture( CST_CRICKET_5,         "cricket5.png" );
   preReadCityTexture( CST_CRICKET_6,         "cricket6.png" );
   preReadCityTexture( CST_CRICKET_7,         "cricket7.png" );
   preReadCityTexture( CST_FIRESTATION_1,       "firestation1.png" );
   preReadCityTexture( CST_FIRESTATION_2,       "firestation2.png" );
   preReadCityTexture( CST_FIRESTATION_3,       "firestation3.png" );
   preReadCityTexture( CST_FIRESTATION_4,       "firestation4.png" );
   preReadCityTexture( CST_FIRESTATION_5,       "firestation5.png" );
   preReadCityTexture( CST_FIRESTATION_6,       "firestation6.png" );
   preReadCityTexture( CST_FIRESTATION_7,       "firestation7.png" );
   preReadCityTexture( CST_FIRESTATION_8,       "firestation8.png" );
   preReadCityTexture( CST_FIRESTATION_9,       "firestation9.png" );
   preReadCityTexture( CST_FIRESTATION_10,      "firestation10.png" );
   preReadCityTexture( CST_TIP_0,             "tip0.png" );
   preReadCityTexture( CST_TIP_1,             "tip1.png" );
   preReadCityTexture( CST_TIP_2,             "tip2.png" );
   preReadCityTexture( CST_TIP_3,             "tip3.png" );
   preReadCityTexture( CST_TIP_4,             "tip4.png" );
   preReadCityTexture( CST_TIP_5,             "tip5.png" );
   preReadCityTexture( CST_TIP_6,             "tip6.png" );
   preReadCityTexture( CST_TIP_7,             "tip7.png" );
   preReadCityTexture( CST_TIP_8,             "tip8.png" );
   preReadCityTexture( CST_COMMUNE_1,         "commune1.png" );
   preReadCityTexture( CST_COMMUNE_2,         "commune2.png" );
   preReadCityTexture( CST_COMMUNE_3,         "commune3.png" );
   preReadCityTexture( CST_COMMUNE_4,         "commune4.png" );
   preReadCityTexture( CST_COMMUNE_5,         "commune5.png" );
   preReadCityTexture( CST_COMMUNE_6,         "commune6.png" );
   preReadCityTexture( CST_COMMUNE_7,         "commune7.png" );
   preReadCityTexture( CST_COMMUNE_8,         "commune8.png" );
   preReadCityTexture( CST_COMMUNE_9,         "commune9.png" );
   preReadCityTexture( CST_COMMUNE_10,        "commune10.png" );
   preReadCityTexture( CST_COMMUNE_11,        "commune11.png" );
   preReadCityTexture( CST_COMMUNE_12,        "commune12.png" );
   preReadCityTexture( CST_COMMUNE_13,        "commune13.png" );
   preReadCityTexture( CST_COMMUNE_14,        "commune14.png" );
   preReadCityTexture( CST_INDUSTRY_H_C,      "industryhc.png" );
   preReadCityTexture( CST_INDUSTRY_H_L1,      "industryhl1.png" );
   preReadCityTexture( CST_INDUSTRY_H_L2,      "industryhl2.png" );
   preReadCityTexture( CST_INDUSTRY_H_L3,      "industryhl3.png" );
   preReadCityTexture( CST_INDUSTRY_H_L4,      "industryhl4.png" );
   preReadCityTexture( CST_INDUSTRY_H_L5,      "industryhl5.png" );
   preReadCityTexture( CST_INDUSTRY_H_L6,      "industryhl6.png" );
   preReadCityTexture( CST_INDUSTRY_H_L7,      "industryhl7.png" );
   preReadCityTexture( CST_INDUSTRY_H_L8,      "industryhl8.png" );
   preReadCityTexture( CST_INDUSTRY_H_M1,      "industryhm1.png" );
   preReadCityTexture( CST_INDUSTRY_H_M2,      "industryhm2.png" );
   preReadCityTexture( CST_INDUSTRY_H_M3,      "industryhm3.png" );
   preReadCityTexture( CST_INDUSTRY_H_M4,      "industryhm4.png" );
   preReadCityTexture( CST_INDUSTRY_H_M5,      "industryhm5.png" );
   preReadCityTexture( CST_INDUSTRY_H_M6,      "industryhm6.png" );
   preReadCityTexture( CST_INDUSTRY_H_M7,      "industryhm7.png" );
   preReadCityTexture( CST_INDUSTRY_H_M8,      "industryhm8.png" );
   preReadCityTexture( CST_INDUSTRY_H_H1,      "industryhh1.png" );
   preReadCityTexture( CST_INDUSTRY_H_H2,      "industryhh2.png" );
   preReadCityTexture( CST_INDUSTRY_H_H3,      "industryhh3.png" );
   preReadCityTexture( CST_INDUSTRY_H_H4,      "industryhh4.png" );
   preReadCityTexture( CST_INDUSTRY_H_H5,      "industryhh5.png" );
   preReadCityTexture( CST_INDUSTRY_H_H6,      "industryhh6.png" );
   preReadCityTexture( CST_INDUSTRY_H_H7,      "industryhh7.png" );
   preReadCityTexture( CST_INDUSTRY_H_H8,      "industryhh8.png" );
   preReadCityTexture( CST_INDUSTRY_L_C,       "industrylq1.png" );
   preReadCityTexture( CST_INDUSTRY_L_Q1,      "industrylq1.png" );
   preReadCityTexture( CST_INDUSTRY_L_Q2,      "industrylq2.png" );
   preReadCityTexture( CST_INDUSTRY_L_Q3,      "industrylq3.png" );
   preReadCityTexture( CST_INDUSTRY_L_Q4,      "industrylq4.png" );
   preReadCityTexture( CST_INDUSTRY_L_L1,      "industryll1.png" );
   preReadCityTexture( CST_INDUSTRY_L_L2,      "industryll2.png" );
   preReadCityTexture( CST_INDUSTRY_L_L3,      "industryll3.png" );
   preReadCityTexture( CST_INDUSTRY_L_L4,      "industryll4.png" );
   preReadCityTexture( CST_INDUSTRY_L_M1,      "industrylm1.png" );
   preReadCityTexture( CST_INDUSTRY_L_M2,      "industrylm2.png" );
   preReadCityTexture( CST_INDUSTRY_L_M3,      "industrylm3.png" );
   preReadCityTexture( CST_INDUSTRY_L_M4,      "industrylm4.png" );
   preReadCityTexture( CST_INDUSTRY_L_H1,      "industrylh1.png" );
   preReadCityTexture( CST_INDUSTRY_L_H2,      "industrylh2.png" );
   preReadCityTexture( CST_INDUSTRY_L_H3,      "industrylh3.png" );
   preReadCityTexture( CST_INDUSTRY_L_H4,      "industrylh4.png" );
   preReadCityTexture( CST_FARM_O0,            "farm0.png" );
   preReadCityTexture( CST_FARM_O1,            "farm1.png" );
   preReadCityTexture( CST_FARM_O2,            "farm2.png" );
   preReadCityTexture( CST_FARM_O3,            "farm3.png" );
   preReadCityTexture( CST_FARM_O4,            "farm4.png" );
   preReadCityTexture( CST_FARM_O5,            "farm5.png" );
   preReadCityTexture( CST_FARM_O6,            "farm6.png" );
   preReadCityTexture( CST_FARM_O7,            "farm7.png" );
   preReadCityTexture( CST_FARM_O8,            "farm8.png" );
   preReadCityTexture( CST_FARM_O9,            "farm9.png" );
   preReadCityTexture( CST_FARM_O10,           "farm10.png" );
   preReadCityTexture( CST_FARM_O11,           "farm11.png" );
   preReadCityTexture( CST_FARM_O12,           "farm12.png" );
   preReadCityTexture( CST_FARM_O13,           "farm13.png" );
   preReadCityTexture( CST_FARM_O14,           "farm14.png" );
   preReadCityTexture( CST_FARM_O15,           "farm15.png" );
   preReadCityTexture( CST_FARM_O16,           "farm16.png" );
   // End of generated Code.
   SDL_mutexV( mThreadRunning );
}

/*
 * Process event
 */
void GameView::event(const Event& event)
{
    float stepx = tileWidth / 2;
    float stepy = tileHeight / 2;
    MapPoint tile;
    Vector2 dragDistance;
    
    switch(event.type) {
        case Event::MOUSEMOTION: {
            if( dragging ) {
                Uint32 now = SDL_GetTicks();
                dragDistance = event.mousepos - dragStart;
                
                int elapsed =  now - dragStartTime;
                if( elapsed < 30 ){ //do nothing if less than 0.03 sec passed.
                    break;
                }
                float dragLength = sqrt(dragDistance.x*dragDistance.x 
                        + dragDistance.y*dragDistance.y);
                float vPixelSec = (1000 * dragLength) / (float) elapsed;
                //std::cout << "v=" << vPixelSec << " Pixels per second\n"; 
                //TODO: sometimes the Distance is way too big, why?
                //std::cout << "dragDistance=" << dragDistance.x << " " << dragDistance.y << "\n"; 
                if( vPixelSec < 2000 ) //if it is faster we just ignore it. TODO: find a better way...
                {  
                    //Mouse Acceleration
                    float accel = 1;
                    //TODO: read Acceleration Parameters from config file.
                    float accelThreshold = 200;
                    float max_accel = 8;
                    
                    if( vPixelSec > accelThreshold ) accel = 1 + ( ( vPixelSec - 200 ) / 100 );
                    if( accel > max_accel ) accel = max_accel;
                    // if( accel < 1 ) accel = 1;
                    //std::cout << "Acceleration: " << accel << "\n"; 
                    
                    dragDistance *= accel;
                    viewport += dragDistance;
                    SDL_WarpMouse( (short unsigned int) dragStart.x, (short unsigned int) dragStart.y );
                }
                dragStartTime = now;
                setDirty();
                break;            
            }         
            if(!event.inside) {
                mouseInGameView = false;
                break;
            }
            mouseInGameView = true;
            if( !dragging && middleButtonDown ) {
                dragging = true;
                dragStart = event.mousepos;
                SDL_ShowCursor( SDL_DISABLE );
                SDL_WM_GrabInput( SDL_GRAB_ON );
                dragStartTime = SDL_GetTicks();
            }         
            MapPoint tile = getTile(event.mousepos);
            if( !roadDragging && leftButtonDown && ( cursorSize == 1 ) ) {
                roadDragging = true;
                startRoad = tile;
            }
 
            if(tileUnderMouse != tile) {
                tileUnderMouse = tile;
                setDirty();
            }
            break;
        }
        case Event::MOUSEBUTTONDOWN: {
            if(!event.inside) {
                break;
            }
            if( event.mousebutton == SDL_BUTTON_MIDDLE ){
                dragging = false;
                middleButtonDown = true;
                break;       
            }
            if( event.mousebutton == SDL_BUTTON_LEFT ){
                roadDragging = false;
                leftButtonDown = true;
                break;       
            }
            break;
        }
        case Event::MOUSEBUTTONUP:
            if( event.mousebutton == SDL_BUTTON_MIDDLE ){
                if ( dragging ) {
                    dragging = false;
                    middleButtonDown = false;
                    SDL_ShowCursor( SDL_ENABLE );
                    SDL_WM_GrabInput( SDL_GRAB_OFF );
                    break;
                } 
                dragging = false;
                middleButtonDown = false;
            }
            if( event.mousebutton == SDL_BUTTON_LEFT ){
                if ( roadDragging && event.inside ) {
                    MapPoint endRoad = getTile( event.mousepos );
                    roadDragging = false;
                    leftButtonDown = false;
                    //use same method to find all Tiles as in void GameView::draw()
                    int stepx = ( startRoad.x > endRoad.x ) ? -1 : 1;
                    int stepy = ( startRoad.y > endRoad.y ) ? -1 : 1;
                    MapPoint currenTile = startRoad;
                    while( currenTile.x != endRoad.x ) {
                        if( !blockingDialogIsOpen )
                            editMap(currenTile, SDL_BUTTON_LEFT);
                        currenTile.x += stepx;
                    }
                    while( currenTile.y != endRoad.y ) {
                        if( !blockingDialogIsOpen )
                            editMap(currenTile, SDL_BUTTON_LEFT);
                        currenTile.y += stepy;
                    }
                    if( !blockingDialogIsOpen )
                        editMap(currenTile, SDL_BUTTON_LEFT);
                    break;
                } 
                roadDragging = false;
                leftButtonDown = false;
            }
            if(!event.inside) {
                break;
            }
            
            tile=getTile( event.mousepos );
            if( event.mousebutton == SDL_BUTTON_LEFT ){              //left
                if( !blockingDialogIsOpen )
                    editMap(tile, SDL_BUTTON_LEFT); //edit tile
            }
            else if( event.mousebutton == SDL_BUTTON_MIDDLE ){  //middle      
                recenter(event.mousepos);                      //adjust view
            }
            else if( event.mousebutton == SDL_BUTTON_RIGHT ){ //right
                getMPS()->setView( tile, MPS_ENV );//show basic info
            }
            else if( event.mousebutton == SDL_BUTTON_WHEELUP ){ //up 
                zoomIn();                                       //zoom in
            }
            else if( event.mousebutton == SDL_BUTTON_WHEELDOWN ){ //down
                zoomOut();                                        //zoom out
            }
            break;
        case Event::KEYUP:
            //Hide High Buildings
            if( event.keysym.sym == SDLK_h ){
                if( hideHigh ){
                    hideHigh = false;
                } else {
                    hideHigh = true;
                }
                requestRedraw();
                break;
            }
            //overlay MiniMap Information
            if( event.keysym.sym == SDLK_v ){
                mapOverlay++;
                if( mapOverlay > overlayMAX ) {
                    mapOverlay = overlayNone;
                }
                requestRedraw();
                break;
            }
            //Zoom
            if( event.keysym.sym == SDLK_KP_PLUS ){
                zoomIn();
                break;
            }
            if( event.keysym.sym == SDLK_KP_MINUS ){
                zoomOut();
                break;
            }
            if( event.keysym.sym == SDLK_KP_ENTER ){
                resetZoom();
                break;
            }
            //Scroll
            if( event.keysym.mod & KMOD_SHIFT ){
                stepx =  5 * tileWidth;
                stepy =  5 * tileHeight;
            } 
            if ( event.keysym.sym == SDLK_KP9 ) {
                viewport.x += stepx;
                viewport.y -= stepy;
                setDirty();
                break;
            }
            if ( event.keysym.sym == SDLK_KP1 ) {
                viewport.x -= stepx;
                viewport.y += stepy;
                setDirty();
                break;
            }
            if ( ( event.keysym.sym == SDLK_KP8 ) || ( event.keysym.sym == SDLK_UP ) ) {
                viewport.y -= stepy;
                setDirty();
                break;
            }
            if ( ( event.keysym.sym == SDLK_KP2 ) || ( event.keysym.sym == SDLK_DOWN ) )  {
                viewport.y += stepy;
                setDirty();
                break;
            }
            if ( event.keysym.sym == SDLK_KP7 ) {
                viewport.x -= stepx;
                viewport.y -= stepy;
                setDirty();
                break;
            }
            if ( event.keysym.sym == SDLK_KP3 ) {
                viewport.x += stepx;
                viewport.y += stepy;
                setDirty();
                break;
            }
            if ( ( event.keysym.sym == SDLK_KP6 ) || ( event.keysym.sym == SDLK_RIGHT ) )  {
                viewport.x += stepx;
                setDirty();
                break;
            }
            if ( ( event.keysym.sym == SDLK_KP4 ) || ( event.keysym.sym == SDLK_LEFT ) ) {
                viewport.x -= stepx;
                setDirty();
                break;
            }
            if ( event.keysym.sym == SDLK_KP5 ) {
                show(MapPoint(WORLD_SIDE_LEN / 2, WORLD_SIDE_LEN / 2));
                setDirty();
                break;
            }
            break;
        default:
            break;
    }
}

/*
 * Parent tells us to change size. 
 */
void GameView::resize(float newwidth , float newheight )
{
    width = newwidth;
    height = newheight;
    requestRedraw();
}

/*
 *  We should draw the whole City again.
 */
void GameView::requestRedraw()
{
    //TODO: do this only when View changed
    //Tell Minimap about new Corners
    getMiniMap()->setGameViewCorners( getTile(Vector2(0, 0)),
            getTile(Vector2(getWidth(), 0)), 
            getTile(Vector2(getWidth(), getHeight())),
            getTile(Vector2(0, getHeight()) ) );  

    //request redraw
    setDirty();
}

/*
 * Pos is new Center of the Screen
 */
void GameView::recenter(const Vector2& pos)
{
    Vector2 position = pos + viewport;
    viewport.x = floor( position.x - ( getWidth() / 2 ) );
    viewport.y = floor( position.y - ( getHeight() / 2 ) );
    
    //request redraw
    requestRedraw();
}

/*
 * Find point on Screen, where lower right corner of tile
 * is placed.
 */
Vector2 GameView::getScreenPoint(MapPoint map)
{
    Vector2 point;
    point.x = virtualScreenWidth / 2 + (map.x - map.y) * ( tileWidth / 2 );
    point.y = (map.x + map.y) * ( tileHeight / 2 ); 
    
    //we want the lower right corner
    point.y += tileHeight;
    //on Screen
    point -= viewport; 

    return point;
}

/*
 * Find Tile at point on viewport
 */
MapPoint GameView::getTile(const Vector2& p)
{
    MapPoint tile;
    // Map Point to virtual Screen
    Vector2 point = p + viewport;
    float x = (point.x - virtualScreenWidth / 2 ) / tileWidth 
        +  point.y  / tileHeight;
    tile.x = (int) floorf(x);
    tile.y = (int) floorf( 2 * point.y  / tileHeight  - x );

    return tile;
}

/*
 * Draw a filled Diamond inside given Rectangle
 */
void GameView::fillDiamond( Painter& painter, const Rect2D& rect )
{
    Vector2 points[ 4 ];
    points[ 0 ].x = rect.p1.x + ( rect.getWidth() / 2 );
    points[ 0 ].y = rect.p1.y;
    points[ 1 ].x = rect.p1.x;
    points[ 1 ].y = rect.p1.y + ( rect.getHeight() / 2 );
    points[ 2 ].x = rect.p1.x + ( rect.getWidth() / 2 );
    points[ 2 ].y = rect.p2.y;
    points[ 3 ].x = rect.p2.x;
    points[ 3 ].y = rect.p1.y + ( rect.getHeight() / 2 );
    painter.fillPolygon( 4, points );    
}

/*
 * Draw a outlined Diamond inside given Rectangle
 */
void GameView::drawDiamond( Painter& painter, const Rect2D& rect )
{
    Vector2 points[ 4 ];
    points[ 0 ].x = rect.p1.x + ( rect.getWidth() / 2 );
    points[ 0 ].y = rect.p1.y;
    points[ 1 ].x = rect.p1.x;
    points[ 1 ].y = rect.p1.y + ( rect.getHeight() / 2 );
    points[ 2 ].x = rect.p1.x + ( rect.getWidth() / 2 );
    points[ 2 ].y = rect.p2.y;
    points[ 3 ].x = rect.p2.x;
    points[ 3 ].y = rect.p1.y + ( rect.getHeight() / 2 );
    painter.drawPolygon( 4, points );    
}

/*
 * Draw MiniMapOverlay for tile.
 */
void GameView::drawOverlay(Painter& painter, MapPoint tile){
    Color black;
    black.parse("black");
    Color miniMapColor;
    
    Vector2 tileOnScreenPoint = getScreenPoint(tile);
    Rect2D tilerect( 0, 0, tileWidth, tileHeight );
    tileOnScreenPoint.x = tileOnScreenPoint.x - ( tileWidth / 2);
    tileOnScreenPoint.y -= tileHeight; 
    tilerect.move( tileOnScreenPoint );         
    //Outside of the Map gets Black overlay
    if( tile.x > WORLD_SIDE_LEN || tile.y > WORLD_SIDE_LEN || tile.x < 0 || tile.y < 0 ) {
            painter.setFillColor( black );
    } else {
        miniMapColor = getMiniMap()->getColor( tile.x, tile.y );
        if( mapOverlay == overlayOn ){
            miniMapColor.a = 200;  //Transparent
        }
        painter.setFillColor( miniMapColor );
    }
    fillDiamond( painter, tilerect );  
}
    
void GameView::drawTile(Painter& painter, MapPoint tile)
{
    Rect2D tilerect( 0, 0, tileWidth, tileHeight );
    Vector2 tileOnScreenPoint = getScreenPoint( tile );

    //is Tile in City? If not draw Blank
    if( tile.x < 0 || tile.y < 0 
            || tile.x >= WORLD_SIDE_LEN || tile.y >= WORLD_SIDE_LEN )
    {
        tileOnScreenPoint.x -= (blankTexture->getWidth() / 2)  * zoom;
        tileOnScreenPoint.y -= blankTexture->getHeight()  * zoom; 
        tilerect.move( tileOnScreenPoint );    
        tilerect.setSize(blankTexture->getWidth() * zoom,
                blankTexture->getHeight() * zoom);
        if(zoom == 1.0) 
        {
            painter.drawTexture( blankTexture, tilerect.p1 );
        }
        else
        {
            painter.drawStretchTexture( blankTexture, tilerect );
        }
        return;
    }

    Texture* texture;
    int size; 
    int upperLeftX = tile.x;
    int upperLeftY = tile.y;    

    if ( MP_TYPE( tile.x, tile.y ) ==  CST_USED ) 
    {
        upperLeftX = MP_INFO(tile.x, tile.y).int_1;
        upperLeftY = MP_INFO(tile.x, tile.y).int_2;    
    }
    size = MP_SIZE( upperLeftX, upperLeftY );

    //is Tile the lower left corner of the Building? 
    //dont't draw if not.
    if ( ( tile.x != upperLeftX ) || ( tile.y - size +1 != upperLeftY ) )
    {
        return;
    }
    //adjust OnScreenPoint of big Tiles
    if( size > 1 ) { 
        if( hideHigh ){ //don't draw big buildings
            return;
        }
        MapPoint lowerRightTile( tile.x + size - 1 , tile.y );
        tileOnScreenPoint = getScreenPoint( lowerRightTile );
    }
    
    int textureType = MP_TYPE( upperLeftX, upperLeftY );
    texture = cityTextures[ textureType ];
    // Test if we have to convert Preloaded Image to Texture
    if( !texture ) {
        SDL_mutexP( mTextures );
        if( cityImages[ textureType ] ){
            cityTextures[ textureType ] = texture_manager->create( cityImages[ textureType ] );
            cityImages[ textureType ] = 0; //Image is erased by texture_manager->create.
            texture = cityTextures[ textureType ];
        }
        SDL_mutexV( mTextures );
    }
    
    if( texture )
    {
        tileOnScreenPoint.x -= cityTextureX[textureType] * zoom;
        tileOnScreenPoint.y -= cityTextureY[textureType] * zoom;  
        tilerect.move( tileOnScreenPoint );    
        tilerect.setSize(texture->getWidth() * zoom, texture->getHeight() * zoom);
        if( zoom == 1.0 ) {
            painter.drawTexture(texture, tilerect.p1);
        }
        else
        {
            painter.drawStretchTexture(texture, tilerect);
        }
    }
    else 
    {
        tileOnScreenPoint.x =  tileOnScreenPoint.x - ( tileWidth / 2);
        tileOnScreenPoint.y -= tileHeight; 
        tilerect.move( tileOnScreenPoint );    
        painter.setFillColor( Color(255, 0, 0, 255) );
        fillDiamond( painter, tilerect );    
    }
}

/*
 * Mark a tile with current cursor
 */
void GameView::markTile( Painter& painter, MapPoint tile )
{
    Vector2 tileOnScreenPoint = getScreenPoint(tile);
    if( cursorSize == 0 ) {
        Color alphawhite( 255, 255, 255, 128 );
        painter.setLineColor( alphawhite );
        Rect2D tilerect( 0, 0, tileWidth, tileHeight );
        tileOnScreenPoint.x = tileOnScreenPoint.x - ( tileWidth / 2);
        tileOnScreenPoint.y -= tileHeight; 
        tilerect.move( tileOnScreenPoint );    
        drawDiamond( painter, tilerect );    
    } else {
        Color alphablue( 0, 0, 255, 128 );
        Color alphared( 255, 0, 0, 128 );
        painter.setFillColor( alphablue );
        //check if building is allowed here, if not use Red Cursor
        int x = (int) tile.x;
        int y = (int) tile.y;
        //  x + cursorSize -1 >= WORLD_SIDE_LEN  would be the same
        if( x + cursorSize > WORLD_SIDE_LEN || y + cursorSize > WORLD_SIDE_LEN || x < 0 || y < 0 ) {
            painter.setFillColor( alphared );
        } else {
            for( y = (int) tile.y; y < tile.y + cursorSize; y++ ) {
                for( x = (int) tile.x; x < tile.x + cursorSize; x++ ) {
                    if( MP_TYPE( x, y ) != CST_GREEN ) {
                        painter.setFillColor( alphared );
                        y += cursorSize;
                        break;
                    }
                }
            }
        }
        //special conditions for some buildings
        //
        //The Harbour needs a River on the East side.
        if( selected_module_type == CST_EX_PORT ){
            x = (int) tile.x + cursorSize;
            y = (int) tile.y;
            for( y = (int) tile.y; y < tile.y + cursorSize; y++ ) {
                if (!( ( MP_GROUP( x, y ) == GROUP_WATER ) && ( MP_INFO(x,y).flags & FLAG_IS_RIVER ) ) ){
                    painter.setFillColor( alphared );
                }
            }
        }
            
        Rect2D tilerect( 0, 0, tileWidth * cursorSize, tileHeight * cursorSize );
        tileOnScreenPoint.x = tileOnScreenPoint.x - (tileWidth * cursorSize / 2);
        tileOnScreenPoint.y -= tileHeight; 
        tilerect.move( tileOnScreenPoint );    
        fillDiamond( painter, tilerect );    
    }
}

/*
 *  Paint an isometric View of the City in the component.
 */
void GameView::draw(Painter& painter)
{
    //If the centre of the Screen is not Part of the city
    //adjust viewport so it is.
    MapPoint centerTile = getCenter();
    bool outside = false;
    if( centerTile.x < 0 ) {
        centerTile.x = 0;
        outside = true;
    }
    if( centerTile.x >= WORLD_SIDE_LEN ) {
        centerTile.x = WORLD_SIDE_LEN - 1;
        outside = true;
    }
    if( centerTile.y < 0 ) {
        centerTile.y = 0;
        outside = true;
    }
    if( centerTile.y >= WORLD_SIDE_LEN ) {
        centerTile.y = WORLD_SIDE_LEN - 1;
        outside = true;
    }
    if( outside ){
        show( centerTile );
        return;
    }
    //The Corners of The Screen
    //TODO: change here to only draw dirtyRect
    //      dirtyRectangle is the current Clippingarea (if set)
    //      so we shold get clippingArea (as sonn this is implemented) 
    //      and adjust these Vectors:
    Vector2 upperLeft( 0, 0);
    Vector2 upperRight( getWidth(), 0 );
    Vector2 lowerLeft( 0, getHeight() );
    
    //Find visible Tiles
    MapPoint upperLeftTile  = getTile( upperLeft ); 
    MapPoint upperRightTile = getTile( upperRight );
    MapPoint lowerLeftTile  = getTile( lowerLeft ); 
    
    //draw Background
    Color green;
    Rect2D background( 0, 0, getWidth(), getHeight() );
    green.parse( "green" );
    painter.setFillColor( green );
    painter.fillRectangle( background );    

    //draw Tiles
    MapPoint currentTile;
    //Draw some extra tiles depending on the maximal size of a building.
    int extratiles = 7;
    upperLeftTile.x -= extratiles;
    upperRightTile.y -= extratiles;
    upperRightTile.x += extratiles;
    lowerLeftTile.y +=  extratiles;

    if( mapOverlay != overlayOnly ){
        for(int k = 0; k <= 2 * ( lowerLeftTile.y - upperLeftTile.y ); k++ )
        {
            for(int i = 0; i <= upperRightTile.x - upperLeftTile.x; i++ )
            {
                currentTile.x = upperLeftTile.x + i + k / 2 + k % 2;
                currentTile.y = upperLeftTile.y - i + k / 2;
                drawTile( painter, currentTile );
            }
        }
    }
    if( mapOverlay != overlayNone ){
        for(int k = 0; k <= 2 * ( lowerLeftTile.y - upperLeftTile.y ); k++ )
        {
            for(int i = 0; i <= upperRightTile.x - upperLeftTile.x; i++ )
            {
                currentTile.x = upperLeftTile.x + i + k / 2 + k % 2;
                currentTile.y = upperLeftTile.y - i + k / 2;
                drawOverlay( painter, currentTile );
            }
        }
    }
    
    //Mark Tile under Mouse 
    if( mouseInGameView  && !blockingDialogIsOpen ) {
        if( roadDragging ){
            //use same method to find all Tiles as in GameView::event(const Event& event)
            int stepx = ( startRoad.x > tileUnderMouse.x ) ? -1 : 1;
            int stepy = ( startRoad.y > tileUnderMouse.y ) ? -1 : 1;
            MapPoint currenTile = startRoad;
            while( currenTile.x != tileUnderMouse.x ) {
                markTile( painter, currenTile );
                currenTile.x += stepx;
            }
            while( currenTile.y != tileUnderMouse.y ) {
                markTile( painter, currenTile );
                currenTile.y += stepy;
            }
        } 
        markTile( painter, tileUnderMouse );
    }
}

//Register as Component
IMPLEMENT_COMPONENT_FACTORY(GameView)
