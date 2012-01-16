//Exceptions that the library might throw in case of
//error conditions as noted.
#ifndef CONFIG_EXCEPTIONS_HPP
#define CONFIG_EXCEPTIONS_HPP
// Config Class
// Author: Charles Gruenwald III
#include <iostream>
#include <exception>

namespace config
{

//This error gets thrown then the parse is unsuccessful
class parserError: public std::exception
{
    public:
    parserError(const String & leftover)
        :
            m_leftover("parser Error: " + leftover)
    {
    }

    virtual ~parserError() throw() {}

    virtual const char* what() const throw()
    {
        return m_leftover.c_str();
    }

    private:
        String m_leftover;

};

//This error gets thrown when a key is not found
class KeyNotFound: public std::exception
{
    virtual const char* what() const throw()
    {
        return "Key not found.";
    }
};

//This error gets thrown when a key is not found
class FileNotFound: public std::exception
{
public:
    virtual const char* what() const throw()
    {
        return "File not found.";
    }
};


//This error gets thrown then the parse is unsuccessful
class SaveError: public std::exception
{
    public:
    SaveError(const String & error_str)
        :
            m_error("Save Error: " + error_str)
    {
    }

    virtual ~SaveError() throw() {}

    virtual const char* what() const throw()
    {
        return m_error.c_str();
    }

    private:
        String m_error;

};

}
#endif
