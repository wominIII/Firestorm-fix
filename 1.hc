// Sample.HC

U0 Greet(U8 *name)
{
    I64 i;

    "Hello, %s!\n", name;

    for (i = 1; i <= 5; i++)
    {
        "Count = %d\n", i;
    }
}

Greet("zmer");
