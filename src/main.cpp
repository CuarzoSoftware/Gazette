#include <Core/Gazette.h>
#include <CZ/Marco/MApp.h>

int main()
{
    Gazette gazette;

    if (!gazette.marco)
        return 1;

    return gazette.marco->run();
}
