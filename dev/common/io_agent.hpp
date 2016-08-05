#pragma once

#include <common/stuff.hpp>

//
// Сообщения, которые необходимы для взаимодействия IO-агента с внешним миром.
//

// Запрос на загрузку содержимого файла.
struct load_email_request
{
  // Имя файла для загрузки.
  string email_file_;
  // Куда нужно прислать результат.
  mbox_t reply_to_;
};

// Успешный результат загрузки файла.
struct load_email_successed
{
  // Содержимое файла.
  string content_;
};

// Неудачный результат загрузки файла.
struct load_email_failed
{
  // Описание причины неудачи.
  string what_;
};

//
// Сам IO-агент.
//
// Реальный IO-агент наверняка будет использовать асинхронный IO, но для
// целей демонстрации используем простую схему имитации асинхронного IO:
// на каждый запрос отсылаем ответ с некоторой задержкой. Это позволит
// нам имитировать паузы в загрузки содержимого файла, но сам IO-агент
// сможет работать на дефолтном диспетчере не приостанавливая рабочую
// нить этого диспетчера.
//
// Так же этот агент имитирует различные нештатные ситуации:
// - каждый 7-й запрос будет завершаться неудачным результатом
//   (т.е. отсылкой load_email_failed);
// - на каждый 15-й запрос ответ не будет отсылаться вовсе.
//
class io_agent final : public agent_t {
public :
  io_agent( context_t ctx ) : agent_t( ctx ) {
    // Для взаимодействия с внешним миром IO-агент будет использовать
    // именованный mbox.
    so_subscribe( so_environment().create_mbox( "io_agent" ) )
      .event( &io_agent::on_request );
  }

private :
  // Этот счетчик нужен для определения того, как среагировать
  // на очередной запрос.
  int counter_{ 0 };

  void on_request( const load_email_request & msg ) {
    ++counter_;
    if( 0 == (counter_ % 15) )
      {} // Вообще ничего не отсылаем, как будто запрос потерялся
         // где-то по дороге.
    else {
      // Для имитации задержки в выполнении запроса.
      const auto pause = chrono::milliseconds( msg.email_file_.length() * 10 );
      if( 0 == (counter_ % 7) )
        // Пришло время отослать отрицательный результат.
        send_delayed< load_email_failed >( so_environment(),
            msg.reply_to_, pause, "IO-operation failed" );
      else
        send_delayed< load_email_successed >( so_environment(),
            msg.reply_to_, pause, string() );
    }
  }
};

void make_io_agent( environment_t & env ) {
  env.introduce_coop( []( coop_t & coop ) {
    coop.make_agent< io_agent >();
  } );
}

