#include<ctype.h>
#include<fcntl.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/time.h>
#include<sys/wait.h>
#include<unistd.h>

#define COMMAND_LENGTH 514  // 1 for \n and 1 for \0
#define DELIM " \t\n"
#define DISPLAY_PROMPT "mysh> "
#define NO_DISPLAY_PROMPT ""
#define COMMAND_NOT_FOUND_ERROR ": Command not found\n"
#define FOPEN_ERROR "Error: Cannot open file "
#define FORK_FAILED_ERROR "Error: Fork failed\n"
#define LONG_COMMAND_ERROR "Error: Command line too long\n"
#define USAGE_ERROR "Usage: mysh [batchFile]\n"

typedef struct node {
  int jid;
  pid_t pid;
  char* cmd;
  struct node* next;
}node_t;

/*
 * @brief Strips the leading and trailing white space from the command
 *
 * @param command : the command whose whitespace is to be stripped
 *
 * @return the command
 */
char* strip_white_space(char *command) {
  char *end_char;
  int i = 0;
  int j = 0;

  // Strip leading white space
  while (isspace(*command)) {
      command++;
      i++;
  }

  // If the entire string was white spaces: onlu \n\0 would remain
  if (strcmp(command, "\n") == 0) {
    return "\n";
  }

  // Strip trailing white space
  end_char = command + strlen(command) - 1;
  while (end_char > command && isspace(*end_char)) {
    end_char--;
  }
  *(end_char + 1) = 0;

  for (j = 0; j < strlen(command); j++) {
    command[j-i] = command[j];
  }
  command[j-i] = 0;
  return command-i;
}

/* @brief Gets the command to be executed (from the user or batchFile according
 *   to the mode.
 *
 * @param mode : 0 for interactive, 1 for batch
 * @param fp : FILE (mode 1), NULL otherwise
 *
 * @return command obtained. NULL if no command, or exit command
 */
char* get_command(int mode, FILE *fp) {
  char *command = (char*)malloc(COMMAND_LENGTH);
  int retval;
  char *returned;

  const char *prompt = mode == 0 ? DISPLAY_PROMPT : NO_DISPLAY_PROMPT;

  do {
    retval = write(1, prompt, strlen(prompt));
    if (retval < 0) {
      perror("Error: ");
    }
// command[0] = '\0';
    if (mode == 0) {
      returned = fgets(command, COMMAND_LENGTH, stdin);
    } else {
      returned = fgets(command, COMMAND_LENGTH, fp);
    }

    // Check Ctrl - D(interactive)  or Error reading (batch)
    if (returned == NULL) {
      free(command);
      if (mode == 0) {
        write(1, "\n", 1);
      }
      return NULL;
    }

    if (mode == 1) {
      // free(command);
      if (feof(fp)) {
        return NULL;
      }
    }

    // Check EOF
    if (command == NULL) {
      return NULL;
    }

    // Check extra long command. No new line characted in this case
    if (strlen(command) == 513 &&
          strchr(command, '\n') == NULL && returned != NULL) {
      // Echo only part of command
      if (mode == 1) {
        write(1, command, 512);
        write(1, "\n", 1);
      }
      write(2, LONG_COMMAND_ERROR, strlen(LONG_COMMAND_ERROR));

      while (strchr(command, '\n') == NULL && returned != NULL) {
      // Read all the rest of the characters of that long command
      if (mode == 0) {
        returned = fgets(command, COMMAND_LENGTH, stdin);
        fflush(stdin);
      } else {
        returned = fgets(command, COMMAND_LENGTH, fp);
        fflush(fp);
      }
      }
      command[0] = '\n';
      continue;
    }
    if (command[0] == '\n' && mode == 1) {
      write(1, "\n", 1);
    }
  } while (command[0] == '\n' || strlen(command) == 0);

  return command;
}

/*
 * Returns number of digits in a number
 */
int num_digits(unsigned long num) {
  int count = 1;
  while (num >= 10) {
    count++;
    num = num / 10;
  }
  return count;
}

/*
 *
 */
int main(int argc, char *argv[]) {
  // Check for correct number of arguments
  if (argc != 1 && argc != 2) {
    write(2, USAGE_ERROR, strlen(USAGE_ERROR));
    exit(1);
  }

  int mode, retval, count, foreground, i, status, jid = 1;
  FILE *fp;
  char *error, *command, *token, *temp, *display;
  unsigned long time;
  struct timeval  initial, final;

  node_t *head = NULL;
  node_t *curr, *prev, *prev_traversal;

  // Mode that the shell is invoked in: 0 for interactive, 1 for bash
  mode = (argc == 1) ? 0 : 1;

  if (mode == 1) {
    // Open the batchFile
    fp = fopen(argv[1], "r");

    // Check if file is invalid or cannot be opened
    if (fp == NULL) {
      error = (char*)malloc(strlen(FOPEN_ERROR) + strlen(argv[1]) + 2);
      error[0] = '\0';
      strcat(error, FOPEN_ERROR);
      strcat(error, argv[1]);
      strcat(error, "\n");
      write(STDERR_FILENO, error, strlen(error));
      free(error);
      exit(1);
    }
  }

  command = get_command(mode, fp);

  // Process the commands
  while (command != NULL) {
    // Echo command back to user
    if (mode == 1) {
       write(1, command, strlen(command));
    }
    command = strip_white_space(command);

    // Find number of args
    count = 0;
    for (i = 0; command[i] != 0; i++) {
      if (command[i] == ' ') {
        count++;
      }
    }

    // Check for foreground or background job
    if (command[strlen(command)-1] == '&') {
      foreground = 0;

      // There was a space before the &
      if (isspace(command[strlen(command)-2])) {
        command[strlen(command)-2] = '\0';
      } else {  // & was last char of last arg. No space before
        command[strlen(command)-1] = '\0';
      }

      if (strcmp(command, "\n") == 0 || command == 0 ||
          strcmp(command, "") == 0) {
        free(command);
        command = get_command(mode, fp);
        continue;
      }

      // To be used while adding job to background list
      temp = (char*)malloc(strlen(command) + 1);
    } else {
      foreground = 1;
    }

    char *args[count+1];

    // Get the command
    token = strtok(command, DELIM);
    i = 0;
    args[i++] = token == NULL ? NULL : strdup(token);
    if (!foreground) {
      strcpy(temp, args[i-1]);
    }

    while (token != NULL) {
      token = strtok(NULL, DELIM);
      args[i++] = token == NULL ? NULL : strdup(token);

      if (!foreground && token != NULL && strlen(args[i-1]) != 0) {
          strcat(temp, " ");
          strcat(temp, args[i-1]);
        }
    }

    if (strcmp(args[0], "j") == 0 && args[1] == NULL) {
      curr = head;
      prev_traversal = head;
      while (curr != NULL) {
        retval = waitpid(curr->pid, &status, WNOHANG);
        if (retval == 0 && (curr->jid > 0 && curr->jid < jid)) {
          display = (char*)malloc(num_digits(curr->jid) + 5 +
            strlen(curr->cmd));
          sprintf(display, "%d : %s\n", curr->jid, curr->cmd);
          write(1, display, strlen(display));
          free(display);
          prev_traversal = curr;
          curr = curr->next;
        } else {
          if (curr->next == NULL) {
            if (curr == head) {
              head = NULL;
            } else {
              prev_traversal->next = NULL;
            }
          } else {
            if (curr == head) {
              head = head->next;
            } else {
              prev_traversal->next = curr->next;
            }
          }
          free(curr->cmd);
          free(curr);
          curr = prev_traversal->next;
        }
      }
    } else if (strcmp(args[0], "myw") == 0 && args[2] == NULL) {
      // Check if arg is a number
      curr = head;
      prev_traversal = head;
      retval = 0;

      while (curr != NULL) {
        if (curr->jid == atoi(args[1])) {
          retval = 1;

          // Wait for process if it is still running
          if (waitpid(curr->pid, &status, WNOHANG|WUNTRACED) == 0) {
            gettimeofday(&initial, NULL);
            retval = waitpid(curr->pid, &status, 0);
            gettimeofday(&final, NULL);

            time = (final.tv_sec - initial.tv_sec)*1000000L
                   + (final.tv_usec - initial.tv_usec);
            display = (char*)malloc(num_digits(curr->jid) + num_digits(time)
                        + 20);
            strcpy(display, "");
            sprintf(display, "%lu : Job %d terminated\n", time, curr->jid);
            write(1, display, strlen(display));
            free(display);
          } else {
            display = (char*)malloc(num_digits(curr->jid) + 20);
            sprintf(display, "0 : Job %d terminated\n", curr->jid);
            write(1, display, strlen(display));
            free(display);
          }

          // Remove terminated job from list
          if (curr->next == NULL) {
            if (curr == head) {
              head = NULL;
            } else {
              prev_traversal->next = NULL;
            }
          } else {
            if (curr == head) {
              head = head->next;
            } else {
              prev_traversal->next = curr->next;
            }
          }
          // Free the memory taken by this node
          free(curr->cmd);
          free(curr);
          break;
        }
        prev_traversal = curr;
        curr = curr->next;
      }
      if (curr == NULL && retval == 0) {
        if (atoi(args[1]) > jid-1 || atoi(args[1]) < 1) {
          display = (char*)malloc(strlen(args[1]) + num_digits(time)
                      + 15);
          sprintf(display, "Invalid jid %s\n", args[1]);
          write(2, display, strlen(display));
          free(display);
        } else {
          display = (char*)malloc(strlen(args[1]) + num_digits(time)
                      + 21);
          sprintf(display, "0 : Job %s terminated\n", args[1]);
          write(1, display, strlen(display));
          free(display);
        }
      }
    } else if (strcmp(args[0], "exit") == 0 && args[1] == NULL) {
        break;
    } else {
      // Add to list of background processes if this is a background
      retval = fork();

      if (retval < 0) {  // Fork failed. Exit
        write(2, FORK_FAILED_ERROR, strlen(FORK_FAILED_ERROR));
        exit(1);
      } else if (retval == 0) {  // child (new process)
        // Execute child command
        execvp(args[0], args);

        // If execvp returned, there was an error with the command
        write(2, args[0], strlen(args[0]));
        write(2, COMMAND_NOT_FOUND_ERROR, strlen(COMMAND_NOT_FOUND_ERROR));

        // To avoid next command from being read twice in batch mode
        if (mode == 1) {
          free(command);
          command = get_command(mode, fp);
        }
      } else {  // parent goes down this path (main)
        if (foreground == 1) {  // Wait for child to complete if foreground job
          waitpid(retval, &status, 0);
          jid++;
        } else {  // Add it to the background list otherwise
          curr = (node_t*)malloc(sizeof(node_t));
          curr->jid = jid++;
          curr->pid = retval;
          curr->cmd = temp;
          curr->next = NULL;

          if (head == NULL) {
            head = curr;
            prev = curr;
          } else {
            prev = head;
            while (prev->next != NULL) {
              prev = prev->next;
            }
            prev->next = curr;
          }
        }
      }
    }
    // Free mem alloc'd to args
    i = 0;
    while (args[i] != NULL) {
      free(args[i]);
      i++;
    }
    free(command);
    command = get_command(mode, fp);
  }

  // Close the batch file if required
  if (mode == 1) {
    fclose(fp);
  }

  if (head != NULL) {
    curr = head;
    prev_traversal = head;

    while (curr != NULL) {
      // Wait for process if it is still running
      if (waitpid(curr->pid, &status, WNOHANG|WUNTRACED) == 0) {
        waitpid(curr->pid, &status, 0);
      }
      prev_traversal = curr->next;
      free(curr->cmd);
      free(curr);
      curr = prev_traversal;
    }
  }
  return 0;
}
