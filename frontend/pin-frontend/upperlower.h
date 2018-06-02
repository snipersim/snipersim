
#ifndef __UPPERLOWER__
#define __UPPERLOWER__

// https://stackoverflow.com/questions/2169261/c-isupper-function
int isupper(int ch)
{
    return (ch >= 'A' && ch <= 'Z');  // ASCII only - not a good implementation!
}

// https://stackoverflow.com/questions/19300596/implementation-of-tolower-function-in-c
int tolower(int chr)//touches only one character per call
{
    return (chr >='A' && chr<='Z') ? (chr + 32) : (chr);    
}

int isalnum(int chr)
{
    return (chr >= 'A' && chr <= 'Z') || (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9');
}

#endif
