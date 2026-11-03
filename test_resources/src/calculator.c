#include "io.h"
#include "power.h"

/* A small UART calculator for the bare-metal HERE demo environment. */
int main(void) {
    print_str("HERE RISC-V calculator\n");
    print_str("Enter: <number> <+|-|*|/> <number>.\n");

    for (;;) {
        long long lhs;
        long long rhs;
        char op;

        print_str("> ");
        if (scanf("%lld", &lhs) != 1) {
            print_str("Invalid expression. Example: 12 * 7\n");
            continue;
        }
        do {
            op = (char)input_char();
        } while (op == ' ' || op == '\t');
        if (scanf("%lld", &rhs) != 1) {
            print_str("Invalid expression. Example: 12 * 7\n");
            continue;
        }

        print_dec(lhs, 0, ' ');
        uart_putc(' ');
        uart_putc(op);
        uart_putc(' ');
        print_dec(rhs, 0, ' ');
        print_str(" = ");

        switch (op) {
        case '+':
            print_dec(lhs + rhs, 0, ' ');
            break;
        case '-':
            print_dec(lhs - rhs, 0, ' ');
            break;
        case '*':
            print_dec(lhs * rhs, 0, ' ');
            break;
        case '/':
            if (rhs == 0) {
                print_str("division by zero");
                break;
            }
            print_dec(lhs / rhs, 0, ' ');
            break;
        default:
            print_str("unknown operator");
            break;
        }
        print_str("\n");
    }
}
