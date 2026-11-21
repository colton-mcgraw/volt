#include <exception>
#include <iostream>

int main()
{
    try
    {
        // Your code here

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}