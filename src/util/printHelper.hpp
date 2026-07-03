/**
 * Jun 30 Bryce Hart
 * welcome to
               _   _                  
  ___ __ _ ___| |_| | ___   _     _   
 / __/ _` / __| __| |/ _ \_| |_ _| |_ 
| (_| (_| \__ \ |_| |  __/_   _|_   _|
 \___\__,_|___/\__|_|\___| |_|   |_| 
 * an open source project by Bryce Hart 
 * 
 * 
 */

#include <iostream>
#include <array>
#include <string_view>
namespace printHelper{


    namespace detail{
//prints brick design
void printBrick(const bool alternate){
    if(alternate){
        std::cout << "|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|_|" << std::endl;
    }else{
        std::cout << "|_|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|" << std::endl;
    }
}

//prints castle logo with border, incoming strings are always 
void printViewWithBrickBorder(const std::string_view view, const bool alternate){
    std::string_view wall;
    if(alternate){ //alt wall
        wall = "|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|_|";
    }else{ //normal wall
        wall = "|_|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|___|";
    }
        const std::size_t dist = ((wall.size() - view.size()) / 2); //distance needed outside ledges
    std::size_t inc = 0;
    for(std::size_t i = 0; i < wall.size(); i++){
        if(i < dist || (dist + view.size()) <= i){
            std::cout << wall.at(i);
        }else{
            std::cout << view.at(inc);
            inc++;
        }
    }
    std::cout << std::endl;
}


    } //namespace detail

void printLogo(){
    const std::array<std::string_view, 5> castleLogo = {
"               _   _                   ",
"  ___ __ _ ___| |_| | ___   _     _    ",
R"( / __/ _` / __| __| |/ _ \_| |_ _| |_  )",
R"(| (_| (_| \__ \ |_| |  __/_   _|_   _| )",
R"( \___\__,_|___/\__|_|\___| |_|   |_|   )",
    };
    bool flip = false;
    detail::printBrick(!flip);
    for(const std::string_view &line : castleLogo){
        detail::printViewWithBrickBorder(line, flip);
        if(flip){
            flip = !flip;
        }
    }
    detail::printBrick(flip);

}


}